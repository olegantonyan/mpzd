#include "util/loop.h"
#include "util/util.h"

#include <errno.h>
#include <sys/poll.h>
#include <stdlib.h>
#include <string.h>

#define LOG_MODULE "loop"

struct LoopFd {
  int fd;
  short events;
  LoopFdCb cb;
  void *ud;
  bool dead;
  LoopFd *next;
};

struct LoopTimer {
  uint64_t deadline;
  LoopTimerCb cb;
  void *ud;
  bool dead;
  LoopTimer *next;
};

static struct {
  LoopFd *fds;
  LoopTimer *timers;
  struct pollfd *pfd;
  LoopFd **map;
  size_t cap;
  void (*after_cb)(void);
  void (*after_batch)(void);
  bool stop;
} L;

LoopFd *loop_add_fd(int fd, short events, LoopFdCb cb, void *ud) {
  LoopFd *h = xcalloc(1, sizeof(*h));
  h->fd = fd;
  h->events = events;
  h->cb = cb;
  h->ud = ud;
  h->next = L.fds;
  L.fds = h;
  return h;
}

void loop_mod(LoopFd *h, short events) {
  h->events = events;
}

void loop_del(LoopFd *h) {
  if (h) {
    h->dead = true;
  }
}

LoopTimer *loop_timer_new(LoopTimerCb cb, void *ud) {
  LoopTimer *t = xcalloc(1, sizeof(*t));
  t->cb = cb;
  t->ud = ud;
  t->next = L.timers;
  L.timers = t;
  return t;
}

void loop_timer_arm(LoopTimer *t, uint64_t deadline_ms) {
  t->deadline = deadline_ms;
}

void loop_timer_free(LoopTimer *t) {
  if (t) {
    t->dead = true;
  }
}

void loop_set_hooks(void (*after_cb)(void), void (*after_batch)(void)) {
  L.after_cb = after_cb;
  L.after_batch = after_batch;
}

static void after_cb(void) {
  if (L.after_cb) {
    L.after_cb();
  }
}

static void sweep(void) {
  LoopFd **fp = &L.fds;
  while (*fp) {
    LoopFd *h = *fp;
    if (h->dead) {
      *fp = h->next;
      free(h);
    } else {
      fp = &h->next;
    }
  }
  LoopTimer **tp = &L.timers;
  while (*tp) {
    LoopTimer *t = *tp;
    if (t->dead) {
      *tp = t->next;
      free(t);
    } else {
      tp = &t->next;
    }
  }
}

static int poll_timeout(int timeout_ms) {
  uint64_t next = 0;
  for (LoopTimer *t = L.timers; t; t = t->next) {
    if (!t->dead && t->deadline && (!next || t->deadline < next)) {
      next = t->deadline;
    }
  }
  if (!next) {
    return timeout_ms;
  }
  uint64_t now = mono_ms();
  uint64_t wait = next > now ? next - now : 0;
  if (wait > INT32_MAX) {
    wait = INT32_MAX;
  }
  return (timeout_ms >= 0 && (uint64_t)timeout_ms < wait) ? timeout_ms : (int)wait;
}

static void fire_timers(void) {
  uint64_t now = mono_ms();
  for (LoopTimer *t = L.timers; t; t = t->next) {
    if (!t->dead && t->deadline && t->deadline <= now) {
      t->deadline = 0;
      t->cb(t->ud);
      after_cb();
    }
  }
}

bool loop_run_once(int timeout_ms) {
  size_t n = 0;
  for (LoopFd *h = L.fds; h; h = h->next) {
    n += !h->dead;
  }
  if (n > L.cap) {
    L.cap = n * 2;
    L.pfd = xrealloc(L.pfd, L.cap * sizeof(*L.pfd));
    L.map = xrealloc(L.map, L.cap * sizeof(*L.map));
  }
  size_t i = 0;
  for (LoopFd *h = L.fds; h; h = h->next) {
    if (!h->dead) {
      L.pfd[i] = (struct pollfd){ .fd = h->fd, .events = h->events };
      L.map[i++] = h;
    }
  }
  int rc = poll(L.pfd, (nfds_t)n, poll_timeout(timeout_ms));
  if (rc < 0 && errno != EINTR) {
    LOG("poll: %s", strerror(errno));
    return false;
  }
  for (i = 0; rc > 0 && i < n; i++) {
    if (L.pfd[i].revents && !L.map[i]->dead) {
      L.map[i]->cb(L.map[i]->ud, L.pfd[i].revents);
      after_cb();
    }
  }
  fire_timers();
  if (L.after_batch) {
    L.after_batch();
  }
  sweep();
  return true;
}

void loop_run(void) {
  L.stop = false;
  while (!L.stop && loop_run_once(-1)) {
  }
}

void loop_stop(void) {
  L.stop = true;
}

void loop_free(void) {
  for (LoopFd *h = L.fds; h; h = h->next) {
    h->dead = true;
  }
  for (LoopTimer *t = L.timers; t; t = t->next) {
    t->dead = true;
  }
  sweep();
  free(L.pfd);
  free(L.map);
  memset(&L, 0, sizeof(L));
}
