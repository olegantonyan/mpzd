#include "player/audio.h"
#include "player/decoder.h"
#include "player/sink.h"
#include "util/util.h"
#include "util/wake.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOG_MODULE "audio"
#define EVENT_RING 64
#define MAX_BOUNDARIES 4
#define OUTPUT_STALL_MS 5000
#define DRAIN_TIMEOUT_MS 1500
#define JOIN_TIMEOUT_SEC 3

typedef struct {
  uint64_t token;
  char *path;
  uint32_t begin_ms;
  uint32_t end_ms;
  bool stream;
} Item;

typedef struct {
  Decoder *dec;
  Item item;
  PcmFmt out;
  int bpf;
  int64_t skip_until;
  int64_t end_sample;
  int64_t next_sample;
  bool eof;
  char desc[96];
} Source;

typedef struct {
  Item item;
  uint64_t frame_at;
  uint32_t start_ms;
  char desc[96];
} Boundary;

static struct {
  pthread_t thread;
  pthread_mutex_t mu;
  pthread_cond_t cv;
  int efd;
  bool running;
  bool exited;
  char *device;

  bool c_quit;
  char *c_device;
  bool c_stop;
  bool c_play;
  bool c_play_paused;
  uint32_t c_play_start;
  Item c_play_item;
  bool c_next;
  bool c_next_has;
  uint64_t c_next_for;
  Item c_next_item;
  int c_pause;

  AudioEvent ev[EVENT_RING];
  unsigned ev_head;
  unsigned ev_tail;

  atomic_int volume;
  atomic_uint pos_ms;
  atomic_bool abort;
} A = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .efd = -1, .c_pause = -1 };

typedef enum { PCM_OK, PCM_ABORT, PCM_FAIL } WriteResult;

static struct {
  bool open;
  PcmFmt fmt;
  int bpf;
  bool hw_paused;
  uint64_t written;
  Source *cur;
  bool paused;
  bool waiting;
  uint64_t waiting_for;
  bool reopen_on_resume;
  uint32_t reopen_pos;
  Boundary b[MAX_BOUNDARIES];
  int nb;
  Boundary audible;
  bool has_audible;
} T;

static void item_set(Item *dst, const AudioItem *src) {
  free(dst->path);
  dst->token = src->token;
  dst->path = xstrdup(src->path);
  dst->begin_ms = src->begin_ms;
  dst->end_ms = src->end_ms;
  dst->stream = src->stream;
}

static void item_dup(Item *dst, const Item *src) {
  *dst = *src;
  dst->path = src->path ? xstrdup(src->path) : NULL;
}

static void item_clear(Item *it) {
  free(it->path);
  memset(it, 0, sizeof(*it));
}

static void emit_at(AudioEventType type, uint64_t token, const char *text, uint32_t pos_ms) {
  pthread_mutex_lock(&A.mu);
  if (A.ev_tail - A.ev_head < EVENT_RING) {
    AudioEvent *e = &A.ev[A.ev_tail % EVENT_RING];
    e->type = type;
    e->token = token;
    e->pos_ms = pos_ms;
    snprintf(e->text, sizeof(e->text), "%s", text ? text : "");
    A.ev_tail++;
  } else {
    LOG("event ring overflow");
  }
  pthread_mutex_unlock(&A.mu);
  wake_signal(A.efd);
}

static void emit(AudioEventType type, uint64_t token, const char *text) {
  emit_at(type, token, text, 0);
}

static bool aborted(void) {
  return atomic_load(&A.abort);
}

static void source_close(Source *s) {
  if (!s) {
    return;
  }
  decoder_close(s->dec);
  item_clear(&s->item);
  free(s);
}

static Source *source_open(const Item *it, uint32_t start_ms) {
  int64_t target = (int64_t)it->begin_ms + start_ms;
  Decoder *dec = decoder_open(it->path, it->stream, it->stream ? 0 : (uint32_t)target, &A.abort);
  if (!dec) {
    return NULL;
  }
  Source *s = xcalloc(1, sizeof(*s));
  s->dec = dec;
  item_dup(&s->item, it);
  s->out = decoder_format(dec);
  s->bpf = pcm_bpf(s->out);
  s->next_sample = -1;
  s->skip_until = it->stream ? 0 : target * s->out.rate / 1000;
  s->end_sample = it->end_ms ? (int64_t)it->end_ms * s->out.rate / 1000 : 0;
  snprintf(s->desc, sizeof(s->desc), "%s", decoder_desc(dec));
  return s;
}

static int source_pull(Source *s, uint8_t **out) {
  for (;;) {
    if (s->eof) {
      return 0;
    }
    uint8_t *data = NULL;
    int64_t pos = -1;
    int n = decoder_read(s->dec, &data, &pos);
    if (n <= 0) {
      return n;
    }
    if (pos < 0) {
      pos = s->next_sample >= 0 ? s->next_sample : s->skip_until;
    }
    s->next_sample = pos + n;
    int64_t off = 0;
    if (pos < s->skip_until) {
      off = s->skip_until - pos;
      if (off >= n) {
        continue;
      }
    }
    int64_t count = n - off;
    if (s->end_sample) {
      int64_t keep = s->end_sample - (pos + off);
      if (keep <= 0) {
        s->eof = true;
        return 0;
      }
      if (keep <= count) {
        count = keep;
        s->eof = true;
      }
    }
    *out = data + off * s->bpf;
    return (int)count;
  }
}

static void out_close(void) {
  sink_close();
  T.open = false;
  T.hw_paused = false;
  T.written = 0;
}

static bool out_open(PcmFmt f) {
  out_close();
  if (!sink_open(A.device, f)) {
    return false;
  }
  T.open = true;
  T.fmt = f;
  T.bpf = pcm_bpf(f);
  return true;
}

static bool out_ready(PcmFmt f) {
  if (T.open && pcm_same(T.fmt, f)) {
    T.hw_paused = false;
    T.written = 0;
    return sink_prepare();
  }
  return out_open(f);
}

static void update_position(void);

static WriteResult out_write(const uint8_t *data, int frames) {
  uint64_t stalled_at = 0;
  while (frames > 0) {
    if (aborted()) {
      return PCM_ABORT;
    }
    int n = sink_write(data, frames);
    if (n < 0) {
      return PCM_FAIL;
    }
    if (n == 0) {
      uint64_t now = mono_ms();
      if (!stalled_at) {
        stalled_at = now;
      } else if (now - stalled_at > OUTPUT_STALL_MS) {
        LOG("output %s stalled", A.device);
        return PCM_FAIL;
      }
      if (!sink_wait(100)) {
        return PCM_FAIL;
      }
      continue;
    }
    stalled_at = 0;
    data += (size_t)n * (size_t)T.bpf;
    frames -= n;
    T.written += (uint64_t)n;
    update_position();
  }
  return PCM_OK;
}

static void apply_volume(uint8_t *data, int frames, PcmFmt f) {
  int v = atomic_load(&A.volume);
  if (v >= 100) {
    return;
  }
  int64_t gain = (int64_t)v * v * 65536 / 10000;
  size_t samples = (size_t)frames * (size_t)f.channels;
  if (f.fmt == SF_S32) {
    int32_t *s = (int32_t *)data;
    for (size_t i = 0; i < samples; i++) {
      s[i] = (int32_t)(((int64_t)s[i] * gain) >> 16);
    }
  } else {
    int16_t *s = (int16_t *)data;
    for (size_t i = 0; i < samples; i++) {
      s[i] = (int16_t)(((int32_t)s[i] * gain) >> 16);
    }
  }
}

static void boundaries_clear(void) {
  for (int i = 0; i < T.nb; i++) {
    item_clear(&T.b[i].item);
  }
  T.nb = 0;
}

static void boundary_push(const Item *it, uint64_t frame_at, uint32_t start_ms, const char *desc) {
  if (T.nb == MAX_BOUNDARIES) {
    item_clear(&T.b[0].item);
    memmove(T.b, T.b + 1, sizeof(Boundary) * (MAX_BOUNDARIES - 1));
    T.nb--;
  }
  Boundary *b = &T.b[T.nb++];
  item_dup(&b->item, it);
  b->frame_at = frame_at;
  b->start_ms = start_ms;
  snprintf(b->desc, sizeof(b->desc), "%s", desc);
}

static void update_position(void) {
  if (!T.open) {
    return;
  }
  int64_t played = (int64_t)T.written - sink_delay();
  if (played < 0) {
    played = 0;
  }
  while (T.nb && played >= (int64_t)T.b[0].frame_at) {
    item_clear(&T.audible.item);
    T.audible = T.b[0];
    T.has_audible = true;
    memmove(T.b, T.b + 1, sizeof(Boundary) * (size_t)(T.nb - 1));
    T.nb--;
    atomic_store(&A.pos_ms, T.audible.start_ms);
    emit(AEV_STARTED, T.audible.item.token, T.audible.desc);
  }
  if (T.has_audible) {
    int64_t ms = T.audible.start_ms + (played - (int64_t)T.audible.frame_at) * 1000 / T.fmt.rate;
    atomic_store(&A.pos_ms, (unsigned)(ms < 0 ? 0 : ms));
  }
}

static void reset_playback(void) {
  source_close(T.cur);
  T.cur = NULL;
  boundaries_clear();
  T.waiting = false;
  T.paused = false;
  T.reopen_on_resume = false;
}

static void do_stop(void) {
  reset_playback();
  out_close();
  item_clear(&T.audible.item);
  T.has_audible = false;
  atomic_store(&A.pos_ms, 0);
}

static void wait_for_next(uint64_t token) {
  T.waiting = true;
  T.waiting_for = token;
}

static void output_failed(uint64_t token, uint32_t pos_ms) {
  do_stop();
  emit_at(AEV_OUTPUT_ERROR, token, A.device, pos_ms);
}

static void do_play(const Item *it, uint32_t start_ms, bool paused) {
  reset_playback();
  if (T.open) {
    sink_drop();
    T.hw_paused = false;
  }
  Source *s = source_open(it, start_ms);
  if (!s) {
    out_close();
    if (!aborted()) {
      emit(AEV_ERROR, it->token, "cannot play");
      wait_for_next(it->token);
    }
    return;
  }
  if (!out_ready(s->out)) {
    source_close(s);
    output_failed(it->token, start_ms);
    return;
  }
  boundary_push(it, 0, start_ms, s->desc);
  T.cur = s;
  T.paused = paused;
  atomic_store(&A.pos_ms, start_ms);
}

static void finish_output(void) {
  if (T.open && !T.paused && sink_drain()) {
    uint64_t deadline = mono_ms() + DRAIN_TIMEOUT_MS;
    while (sink_draining()) {
      if (aborted() || mono_ms() > deadline) {
        sink_drop();
        break;
      }
      update_position();
      struct timespec ts = { 0, 20 * 1000000L };
      nanosleep(&ts, NULL);
    }
  }
  update_position();
}

static void do_next(const Item *it, uint64_t for_token) {
  if (!T.waiting || for_token != T.waiting_for) {
    return;
  }
  T.waiting = false;
  if (!it) {
    uint64_t last = T.has_audible ? T.audible.item.token : for_token;
    finish_output();
    do_stop();
    emit(AEV_STOPPED, last, NULL);
    return;
  }
  Source *s = source_open(it, 0);
  if (!s) {
    if (!aborted()) {
      emit(AEV_ERROR, it->token, "cannot play");
      wait_for_next(it->token);
    }
    return;
  }
  if (!T.open || !pcm_same(T.fmt, s->out)) {
    finish_output();
    if (!out_open(s->out)) {
      source_close(s);
      output_failed(it->token, 0);
      return;
    }
  }
  boundary_push(it, T.written, 0, s->desc);
  T.cur = s;
}

static void do_pause(bool pause) {
  if (pause == T.paused || (!T.cur && !T.waiting)) {
    return;
  }
  if (pause) {
    update_position();
    if (T.open && sink_running()) {
      if (sink_pause(true)) {
        T.hw_paused = true;
      } else {
        T.reopen_on_resume = T.has_audible;
        T.reopen_pos = atomic_load(&A.pos_ms);
        sink_drop();
      }
    }
    T.paused = true;
    return;
  }
  T.paused = false;
  if (T.hw_paused) {
    sink_pause(false);
    T.hw_paused = false;
  } else if (T.reopen_on_resume) {
    Item it;
    item_dup(&it, &T.audible.item);
    do_play(&it, T.reopen_pos, false);
    item_clear(&it);
  }
}

static void do_set_device(char *device) {
  free(A.device);
  A.device = device;
  if (!T.open) {
    return;
  }
  if (T.reopen_on_resume || !T.has_audible) {
    out_close();
    return;
  }
  update_position();
  out_close();
  Item it;
  item_dup(&it, &T.audible.item);
  do_play(&it, atomic_load(&A.pos_ms), T.paused);
  item_clear(&it);
}

static void step(void) {
  uint8_t *data = NULL;
  int frames = source_pull(T.cur, &data);
  char icy[sizeof(((AudioEvent *)0)->text)];
  if (T.cur->item.stream && decoder_icy(T.cur->dec, icy, sizeof(icy))) {
    emit(AEV_ICY, T.cur->item.token, icy);
  }
  if (frames > 0) {
    apply_volume(data, frames, T.cur->out);
    if (out_write(data, frames) == PCM_FAIL) {
      update_position();
      output_failed(T.has_audible ? T.audible.item.token : T.cur->item.token, atomic_load(&A.pos_ms));
    }
    return;
  }
  if (aborted()) {
    return;
  }
  uint64_t token = T.cur->item.token;
  source_close(T.cur);
  T.cur = NULL;
  wait_for_next(token);
  emit(AEV_NEED_NEXT, token, NULL);
}

static bool has_command(void) {
  return A.c_quit || A.c_device || A.c_stop || A.c_play || A.c_next || A.c_pause >= 0;
}

static void *audio_main(void *arg) {
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&A.mu);
    while (!has_command() && !(T.cur && !T.paused && !T.waiting)) {
      if (T.open && !T.paused && (T.waiting || T.nb)) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 20 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
          ts.tv_sec++;
          ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&A.cv, &A.mu, &ts);
        break;
      }
      pthread_cond_wait(&A.cv, &A.mu);
    }
    bool quit = A.c_quit;
    bool stop = A.c_stop;
    bool play = A.c_play;
    bool play_paused = A.c_play_paused;
    uint32_t play_start = A.c_play_start;
    Item play_item = A.c_play_item;
    bool next = A.c_next;
    bool next_has = A.c_next_has;
    uint64_t next_for = A.c_next_for;
    Item next_item = A.c_next_item;
    int pause = A.c_pause;
    char *device = A.c_device;
    A.c_device = NULL;
    atomic_store(&A.abort, false);
    memset(&A.c_play_item, 0, sizeof(Item));
    memset(&A.c_next_item, 0, sizeof(Item));
    A.c_stop = A.c_play = A.c_next = false;
    A.c_pause = -1;
    pthread_mutex_unlock(&A.mu);

    if (quit) {
      item_clear(&play_item);
      item_clear(&next_item);
      free(device);
      break;
    }
    if (device) {
      do_set_device(device);
    }
    if (stop) {
      do_stop();
    }
    if (play) {
      do_play(&play_item, play_start, play_paused);
    } else if (next) {
      do_next(next_has ? &next_item : NULL, next_for);
    }
    if (pause >= 0) {
      do_pause(pause == 1);
    }
    item_clear(&play_item);
    item_clear(&next_item);

    if (T.cur && !T.paused && !T.waiting) {
      step();
    }
    update_position();
  }
  do_stop();
  pthread_mutex_lock(&A.mu);
  A.exited = true;
  pthread_cond_broadcast(&A.cv);
  pthread_mutex_unlock(&A.mu);
  return NULL;
}

bool audio_init(const char *device, int *event_fd) {
  A.device = xstrdup(device);
  atomic_store(&A.volume, 100);
  A.efd = wake_open();
  if (A.efd < 0) {
    return false;
  }
  if (pthread_create(&A.thread, NULL, audio_main, NULL) != 0) {
    wake_close(A.efd);
    return false;
  }
  A.running = true;
  *event_fd = A.efd;
  return true;
}

void audio_shutdown(void) {
  if (!A.running) {
    return;
  }
  pthread_mutex_lock(&A.mu);
  A.c_quit = true;
  atomic_store(&A.abort, true);
  pthread_cond_signal(&A.cv);
  pthread_mutex_unlock(&A.mu);
  A.running = false;
  if (!join_within(A.thread, &A.mu, &A.cv, &A.exited, JOIN_TIMEOUT_SEC)) {
    LOG("audio thread stuck, not waiting for it");
    return;
  }
  item_clear(&A.c_play_item);
  item_clear(&A.c_next_item);
  item_clear(&T.audible.item);
  wake_close(A.efd);
  A.efd = -1;
  free(A.device);
  A.device = NULL;
  free(A.c_device);
  A.c_device = NULL;
}

void audio_set_device(const char *device) {
  pthread_mutex_lock(&A.mu);
  free(A.c_device);
  A.c_device = xstrdup(device);
  pthread_cond_signal(&A.cv);
  pthread_mutex_unlock(&A.mu);
}

void audio_play(const AudioItem *item, uint32_t start_ms, bool paused) {
  pthread_mutex_lock(&A.mu);
  item_set(&A.c_play_item, item);
  A.c_play = true;
  A.c_play_start = start_ms;
  A.c_play_paused = paused;
  atomic_store(&A.abort, true);
  A.c_stop = false;
  A.c_next = false;
  A.c_pause = -1;
  item_clear(&A.c_next_item);
  pthread_cond_signal(&A.cv);
  pthread_mutex_unlock(&A.mu);
}

void audio_next(const AudioItem *item, uint64_t for_token) {
  pthread_mutex_lock(&A.mu);
  if (!A.c_play && !A.c_stop) {
    A.c_next = true;
    A.c_next_for = for_token;
    A.c_next_has = item != NULL;
    if (item) {
      item_set(&A.c_next_item, item);
    } else {
      item_clear(&A.c_next_item);
    }
    pthread_cond_signal(&A.cv);
  }
  pthread_mutex_unlock(&A.mu);
}

void audio_pause(bool pause) {
  pthread_mutex_lock(&A.mu);
  A.c_pause = pause ? 1 : 0;
  pthread_cond_signal(&A.cv);
  pthread_mutex_unlock(&A.mu);
}

void audio_stop(void) {
  pthread_mutex_lock(&A.mu);
  A.c_stop = true;
  atomic_store(&A.abort, true);
  A.c_play = false;
  A.c_next = false;
  A.c_pause = -1;
  item_clear(&A.c_play_item);
  item_clear(&A.c_next_item);
  pthread_cond_signal(&A.cv);
  pthread_mutex_unlock(&A.mu);
}

void audio_set_volume(int volume) {
  atomic_store(&A.volume, volume < 0 ? 0 : (volume > 100 ? 100 : volume));
}

bool audio_poll_event(AudioEvent *ev) {
  pthread_mutex_lock(&A.mu);
  bool got = A.ev_head != A.ev_tail;
  if (got) {
    *ev = A.ev[A.ev_head % EVENT_RING];
    A.ev_head++;
  }
  pthread_mutex_unlock(&A.mu);
  return got;
}

void audio_ack_events(void) {
  wake_drain(A.efd);
}

uint32_t audio_position_ms(void) {
  return atomic_load(&A.pos_ms);
}
