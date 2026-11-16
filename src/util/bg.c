#include "util/bg.h"
#include "util/loop.h"
#include "util/util.h"
#include "util/wake.h"

#include <sys/poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define LOG_MODULE "bg"
#define BG_MAX_THREADS 16

typedef struct Job {
  void *arg;
  BgWork work;
  BgDone done;
  BgFree free_fn;
  uint64_t deadline;
  bool finished;
  bool abandoned;
  atomic_int refs;
  struct Job *next;
} Job;

static struct {
  pthread_mutex_t mu;
  Job *jobs;
  int running;
  int efd;
  LoopFd *ev;
  LoopTimer *timer;
} B = { .mu = PTHREAD_MUTEX_INITIALIZER, .efd = -1 };

static void unref(Job *j) {
  if (atomic_fetch_sub(&j->refs, 1) == 1) {
    j->free_fn(j->arg);
    free(j);
  }
}

static void *worker(void *p) {
  Job *j = p;
  j->work(j->arg);
  pthread_mutex_lock(&B.mu);
  B.running--;
  j->finished = true;
  bool abandoned = j->abandoned;
  pthread_mutex_unlock(&B.mu);
  unref(j);
  if (!abandoned) {
    wake_signal(B.efd);
  }
  return NULL;
}

static void arm_timer(void) {
  uint64_t next = 0;
  pthread_mutex_lock(&B.mu);
  for (Job *j = B.jobs; j; j = j->next) {
    if (!j->finished && (!next || j->deadline < next)) {
      next = j->deadline;
    }
  }
  pthread_mutex_unlock(&B.mu);
  if (B.timer) {
    loop_timer_arm(B.timer, next);
  }
}

static void on_event(void *ud, short revents);
static void on_timer(void *ud);

bool bg_init(void) {
  B.efd = wake_open();
  if (B.efd < 0) {
    return false;
  }
  B.ev = loop_add_fd(B.efd, POLLIN, on_event, NULL);
  B.timer = loop_timer_new(on_timer, NULL);
  return true;
}

bool bg_run(void *arg, BgWork work, BgDone done, BgFree free_fn, unsigned timeout_ms) {
  pthread_mutex_lock(&B.mu);
  bool full = B.running >= BG_MAX_THREADS;
  if (!full) {
    B.running++;
  }
  pthread_mutex_unlock(&B.mu);
  if (full) {
    LOG("too many stuck jobs");
    return false;
  }
  Job *j = xcalloc(1, sizeof(*j));
  j->arg = arg;
  j->work = work;
  j->done = done;
  j->free_fn = free_fn;
  j->deadline = mono_ms() + timeout_ms;
  atomic_init(&j->refs, 2);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_mutex_lock(&B.mu);
  j->next = B.jobs;
  B.jobs = j;
  pthread_t t;
  bool ok = pthread_create(&t, &attr, worker, j) == 0;
  if (!ok) {
    B.jobs = j->next;
    B.running--;
  }
  pthread_mutex_unlock(&B.mu);
  pthread_attr_destroy(&attr);
  if (!ok) {
    LOG("cannot start thread");
    free(j);
    return false;
  }
  arm_timer();
  return true;
}

static Job *take(bool expired_only, uint64_t now) {
  Job *out = NULL;
  pthread_mutex_lock(&B.mu);
  Job **pp = &B.jobs;
  while (*pp) {
    Job *j = *pp;
    bool pick = expired_only ? (!j->finished && j->deadline <= now) : j->finished;
    if (pick) {
      if (expired_only) {
        j->abandoned = true;
      }
      *pp = j->next;
      j->next = out;
      out = j;
    } else {
      pp = &j->next;
    }
  }
  pthread_mutex_unlock(&B.mu);
  return out;
}

static void deliver(Job *list, bool timed_out) {
  while (list) {
    Job *next = list->next;
    list->done(list->arg, timed_out);
    unref(list);
    list = next;
  }
}

static void on_event(void *ud, short revents) {
  wake_drain(B.efd);
  deliver(take(false, 0), false);
  arm_timer();
}

static void on_timer(void *ud) {
  deliver(take(true, mono_ms()), true);
  arm_timer();
}

void bg_shutdown(void) {
  deliver(take(true, UINT64_MAX), true);
  deliver(take(false, 0), false);
  loop_del(B.ev);
  loop_timer_free(B.timer);
  wake_close(B.efd);
  B.ev = NULL;
  B.timer = NULL;
  B.efd = -1;
}
