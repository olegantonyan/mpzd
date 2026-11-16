#include "library/scan.h"
#include "library/cue.h"
#include "library/m3u.h"
#include "library/tags.h"
#include "model/sort.h"

#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_MODULE "scan"
#define STALL_MS 30000
#define JOIN_TIMEOUT_SEC 3

static void progress(const char *path);

static void add_stream(const char *url, TrackVec *out) {
  TrackInfo info = { .str = { [TS_PATH] = url }, .flags = TRACK_STREAM };
  Track t;
  track_init(&t, &info);
  trackvec_push(out, &t);
}

static void add_file(const char *path, TrackVec *out) {
  progress(path);
  Track t;
  if (tags_read(path, &t)) {
    trackvec_push(out, &t);
  }
}

static void walk(const char *dir, StrVec *audio, StrVec *cues) {
  DIR *d = opendir(dir);
  if (!d) {
    return;
  }
  StrBuf p = { 0 };
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') {
      continue;
    }
    sb_reset(&p);
    sb_puts(&p, dir);
    sb_puts(&p, "/");
    sb_puts(&p, e->d_name);
    progress(p.data);
    unsigned char type = e->d_type;
    if (type == DT_UNKNOWN || type == DT_LNK) {
      struct stat st;
      if (lstat(p.data, &st) != 0) {
        continue;
      }
      if (S_ISLNK(st.st_mode)) {
        if (stat(p.data, &st) != 0 || S_ISDIR(st.st_mode)) {
          continue;
        }
      }
      type = S_ISDIR(st.st_mode) ? DT_DIR : (S_ISREG(st.st_mode) ? DT_REG : DT_UNKNOWN);
    }
    if (type == DT_DIR) {
      walk(p.data, audio, cues);
    } else if (type == DT_REG) {
      if (is_cue_file(e->d_name)) {
        strvec_push(cues, xstrdup(p.data));
      } else if (is_audio_file(e->d_name)) {
        strvec_push(audio, xstrdup(p.data));
      }
    }
  }
  sb_free(&p);
  closedir(d);
}

static int cmp_str(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

static void scan_dir(const char *dir, TrackVec *out) {
  StrVec audio = { 0 };
  StrVec cues = { 0 };
  StrVec referenced = { 0 };
  walk(dir, &audio, &cues);

  TrackVec found = { 0 };
  for (size_t i = 0; i < cues.len; i++) {
    progress(cues.v[i]);
    cue_parse(cues.v[i], tags_duration_ms, &found, &referenced);
  }
  if (referenced.len) {
    qsort(referenced.v, referenced.len, sizeof(char *), cmp_str);
  }
  for (size_t i = 0; i < audio.len; i++) {
    if (referenced.len && bsearch(&audio.v[i], referenced.v, referenced.len, sizeof(char *), cmp_str)) {
      continue;
    }
    add_file(audio.v[i], &found);
  }

  sort_tracks_default(found.v, found.len);
  trackvec_append(out, &found);
  strvec_free(&audio);
  strvec_free(&cues);
  strvec_free(&referenced);
}

static void scan_playlist_file(const char *path, TrackVec *out) {
  StrVec entries = { 0 };
  m3u_parse(path, &entries);
  for (size_t i = 0; i < entries.len; i++) {
    const char *e = entries.v[i];
    if (path_is_url(e)) {
      add_stream(e, out);
    } else if (is_audio_file(e)) {
      char *real = realpath(e, NULL);
      if (real) {
        add_file(real, out);
        free(real);
      }
    }
  }
  strvec_free(&entries);
}

void scan_path(const char *path, TrackVec *out) {
  if (path_is_url(path)) {
    add_stream(path, out);
    return;
  }
  struct stat st;
  if (stat(path, &st) != 0) {
    return;
  }
  if (S_ISDIR(st.st_mode)) {
    scan_dir(path, out);
  } else if (is_playlist_file(path)) {
    scan_playlist_file(path, out);
  } else if (is_cue_file(path)) {
    StrVec files = { 0 };
    TrackVec found = { 0 };
    cue_parse(path, tags_duration_ms, &found, &files);
    sort_tracks_default(found.v, found.len);
    trackvec_append(out, &found);
    strvec_free(&files);
  } else if (is_audio_file(path)) {
    add_file(path, out);
  }
}

void scan_reload(TrackVec *tracks) {
  for (size_t i = 0; i < tracks->len; i++) {
    Track *t = &tracks->v[i];
    if (t->flags & (TRACK_STREAM | TRACK_CUE)) {
      continue;
    }
    Track fresh;
    progress(track_str(t, TS_PATH));
    if (tags_read(track_str(t, TS_PATH), &fresh)) {
      fresh.id = t->id;
      track_free(t);
      *t = fresh;
    }
  }
}

static struct {
  pthread_t thread;
  pthread_mutex_t mu;
  pthread_cond_t cv;
  ScanJob *todo_head;
  ScanJob *todo_tail;
  ScanJob *done_head;
  ScanJob *done_tail;
  int efd;
  bool quit;
  bool running;
  unsigned gen;
  ScanJob *current;
  uint64_t progress_at;
  char where[PATH_MAX];
} S = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .efd = -1 };

static void progress(const char *path) {
  pthread_mutex_lock(&S.mu);
  S.progress_at = mono_ms();
  snprintf(S.where, sizeof(S.where), "%s", path);
  pthread_mutex_unlock(&S.mu);
}

static void push_done(ScanJob *job) {
  job->next = NULL;
  if (S.done_tail) {
    S.done_tail->next = job;
  } else {
    S.done_head = job;
  }
  S.done_tail = job;
  uint64_t one = 1;
  if (write(S.efd, &one, sizeof(one)) < 0) {
    LOG("eventfd write failed");
  }
}

static void run_job(ScanJob *job) {
  if (job->kind == SCAN_RELOAD) {
    scan_reload(&job->tracks);
    return;
  }
  for (size_t i = 0; i < job->paths.len; i++) {
    scan_path(job->paths.v[i], &job->tracks);
  }
}

static void *worker(void *arg) {
  unsigned gen = (unsigned)(uintptr_t)arg;
  pthread_mutex_lock(&S.mu);
  while (!S.quit && gen == S.gen) {
    ScanJob *job = S.todo_head;
    if (!job) {
      pthread_cond_wait(&S.cv, &S.mu);
      continue;
    }
    S.todo_head = job->next;
    if (!S.todo_head) {
      S.todo_tail = NULL;
    }
    S.current = job;
    S.progress_at = mono_ms();
    pthread_mutex_unlock(&S.mu);

    run_job(job);

    pthread_mutex_lock(&S.mu);
    if (gen != S.gen) {
      pthread_mutex_unlock(&S.mu);
      scanjob_free(job);
      return NULL;
    }
    S.current = NULL;
    push_done(job);
  }
  pthread_mutex_unlock(&S.mu);
  return NULL;
}

static bool spawn(void) {
  return pthread_create(&S.thread, NULL, worker, (void *)(uintptr_t)S.gen) == 0;
}

void scanner_watchdog(void) {
  pthread_mutex_lock(&S.mu);
  if (!S.current || mono_ms() - S.progress_at < STALL_MS) {
    pthread_mutex_unlock(&S.mu);
    return;
  }
  ScanJob *stuck = S.current;
  LOG("scan of playlist %u stalled on %s, abandoned", stuck->pl_id, S.where);
  ScanJob *job = xcalloc(1, sizeof(*job));
  job->kind = stuck->kind;
  job->pl_id = stuck->pl_id;
  job->at = stuck->at;
  S.current = NULL;
  S.gen++;
  push_done(job);
  pthread_detach(S.thread);
  pthread_cond_broadcast(&S.cv);
  if (!spawn()) {
    LOG("cannot restart scanner");
    S.running = false;
  }
  pthread_mutex_unlock(&S.mu);
}

bool scanner_start(int *event_fd) {
  S.efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (S.efd < 0) {
    return false;
  }
  if (!spawn()) {
    close(S.efd);
    return false;
  }
  S.running = true;
  *event_fd = S.efd;
  return true;
}

void scanner_stop(void) {
  if (!S.running) {
    return;
  }
  pthread_mutex_lock(&S.mu);
  S.quit = true;
  pthread_cond_broadcast(&S.cv);
  pthread_mutex_unlock(&S.mu);
  S.running = false;
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += JOIN_TIMEOUT_SEC;
  if (pthread_timedjoin_np(S.thread, NULL, &ts) != 0) {
    pthread_mutex_lock(&S.mu);
    LOG("scanner stuck on %s, not waiting for it", S.where);
    pthread_mutex_unlock(&S.mu);
    pthread_detach(S.thread);
    return;
  }
  for (ScanJob *j = S.todo_head; j;) {
    ScanJob *next = j->next;
    scanjob_free(j);
    j = next;
  }
  for (ScanJob *j = S.done_head; j;) {
    ScanJob *next = j->next;
    scanjob_free(j);
    j = next;
  }
  S.todo_head = S.todo_tail = S.done_head = S.done_tail = NULL;
  close(S.efd);
  S.efd = -1;
}

void scanner_submit(ScanJob *job) {
  if (!S.running) {
    pthread_mutex_lock(&S.mu);
    push_done(job);
    pthread_mutex_unlock(&S.mu);
    return;
  }
  job->next = NULL;
  pthread_mutex_lock(&S.mu);
  if (S.todo_tail) {
    S.todo_tail->next = job;
  } else {
    S.todo_head = job;
  }
  S.todo_tail = job;
  pthread_cond_signal(&S.cv);
  pthread_mutex_unlock(&S.mu);
}

ScanJob *scanner_take_done(void) {
  uint64_t v;
  if (read(S.efd, &v, sizeof(v)) < 0) {
    v = 0;
  }
  pthread_mutex_lock(&S.mu);
  ScanJob *list = S.done_head;
  S.done_head = S.done_tail = NULL;
  pthread_mutex_unlock(&S.mu);
  return list;
}

void scanjob_free(ScanJob *job) {
  strvec_free(&job->paths);
  trackvec_free(&job->tracks);
  free(job);
}
