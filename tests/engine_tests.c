#include "player/audio.h"
#include "player/decoder.h"
#include "player/sink.h"
#include "util/util.h"

#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond) do { \
  if (!(cond)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    failures++; \
  } \
} while (0)

#define RATE 1000
#define READ_FRAMES 100
#define SEEK_GRAIN 128

typedef struct {
  const char *path;
  SampleFmt fmt;
  int frames;
} FakeTrack;

static const FakeTrack tracks[] = {
  { "a", SF_S32, 1000 },
  { "b", SF_S32, 1000 },
  { "c16", SF_S16, 500 },
  { "long", SF_S32, 1000000 },
};

struct Decoder {
  int id;
  int64_t at;
  int32_t buf32[READ_FRAMES * 2];
  int16_t buf16[READ_FRAMES * 2];
};

static struct {
  pthread_mutex_t mu;
  int decoder_opens;
  int opens;
  int drains;
  bool can_pause;
  bool fail;
  bool open;
  bool paused;
  int max_chunk;
  unsigned sleep_us;
  PcmFmt fmt;
  int32_t *rec;
  size_t nrec;
  size_t cap;
} F = { .mu = PTHREAD_MUTEX_INITIALIZER };

static void fake_reset(void) {
  AudioEvent ev;
  while (audio_poll_event(&ev)) {
  }
  pthread_mutex_lock(&F.mu);
  free(F.rec);
  F.rec = NULL;
  F.nrec = F.cap = 0;
  F.decoder_opens = F.opens = F.drains = 0;
  F.can_pause = F.fail = F.open = F.paused = false;
  F.max_chunk = 1 << 20;
  F.sleep_us = 0;
  pthread_mutex_unlock(&F.mu);
}

static size_t recorded(void) {
  pthread_mutex_lock(&F.mu);
  size_t n = F.nrec;
  pthread_mutex_unlock(&F.mu);
  return n;
}

static int32_t value(int id, int64_t index) {
  return tracks[id].fmt == SF_S32 ? id * 10000000 + (int32_t)index : (int32_t)index;
}

Decoder *decoder_open(const char *path, bool stream, uint32_t seek_ms, const atomic_bool *abort) {
  for (int i = 0; i < (int)ARRAY_LEN(tracks); i++) {
    if (strcmp(tracks[i].path, path) == 0) {
      Decoder *d = xcalloc(1, sizeof(*d));
      d->id = i;
      d->at = (int64_t)seek_ms * RATE / 1000 / SEEK_GRAIN * SEEK_GRAIN;
      pthread_mutex_lock(&F.mu);
      F.decoder_opens++;
      pthread_mutex_unlock(&F.mu);
      return d;
    }
  }
  return NULL;
}

PcmFmt decoder_format(const Decoder *d) {
  return (PcmFmt){ RATE, 2, tracks[d->id].fmt };
}

const char *decoder_desc(const Decoder *d) {
  return tracks[d->id].path;
}

int decoder_read(Decoder *d, uint8_t **out, int64_t *pos) {
  int64_t left = tracks[d->id].frames - d->at;
  if (left <= 0) {
    return 0;
  }
  int n = left < READ_FRAMES ? (int)left : READ_FRAMES;
  for (int i = 0; i < n; i++) {
    int32_t v = value(d->id, d->at + i);
    d->buf32[i * 2] = d->buf32[i * 2 + 1] = v;
    d->buf16[i * 2] = d->buf16[i * 2 + 1] = (int16_t)v;
  }
  *out = tracks[d->id].fmt == SF_S32 ? (uint8_t *)d->buf32 : (uint8_t *)d->buf16;
  *pos = d->at;
  d->at += n;
  return n;
}

bool decoder_icy(Decoder *d, char *buf, size_t n) {
  return false;
}

void decoder_close(Decoder *d) {
  free(d);
}

bool sink_open(const char *device, PcmFmt f) {
  pthread_mutex_lock(&F.mu);
  F.opens++;
  F.open = true;
  F.paused = false;
  F.fmt = f;
  pthread_mutex_unlock(&F.mu);
  return true;
}

bool sink_prepare(void) {
  F.paused = false;
  return true;
}

void sink_drop(void) {
}

void sink_close(void) {
  F.open = false;
}

int sink_write(const uint8_t *data, int frames) {
  pthread_mutex_lock(&F.mu);
  if (F.fail) {
    pthread_mutex_unlock(&F.mu);
    return -1;
  }
  int n = frames < F.max_chunk ? frames : F.max_chunk;
  VEC_RESERVE(F.rec, F.nrec, F.cap, (size_t)n);
  for (int i = 0; i < n; i++) {
    F.rec[F.nrec++] = F.fmt.fmt == SF_S32 ? ((const int32_t *)data)[i * 2] : ((const int16_t *)data)[i * 2];
  }
  unsigned sleep_us = F.sleep_us;
  pthread_mutex_unlock(&F.mu);
  if (sleep_us) {
    usleep(sleep_us);
  }
  return n;
}

bool sink_wait(int timeout_ms) {
  return true;
}

int64_t sink_delay(void) {
  return 0;
}

bool sink_running(void) {
  return F.open && !F.paused;
}

bool sink_pause(bool on) {
  if (!F.can_pause) {
    return false;
  }
  F.paused = on;
  return true;
}

bool sink_drain(void) {
  F.drains++;
  return true;
}

bool sink_draining(void) {
  return false;
}

static int event_fd;

static bool next_event(AudioEvent *ev) {
  uint64_t until = mono_ms() + 2000;
  while (mono_ms() < until) {
    if (audio_poll_event(ev)) {
      return true;
    }
    struct pollfd p = { .fd = event_fd, .events = POLLIN };
    if (poll(&p, 1, 50) > 0) {
      audio_ack_events();
    }
  }
  return false;
}

static bool expect(AudioEventType type, uint64_t token) {
  AudioEvent ev;
  bool ok = next_event(&ev) && ev.type == type && ev.token == token;
  if (!ok) {
    fprintf(stderr, "expected event %d token %llu\n", type, (unsigned long long)token);
  }
  return ok;
}

static AudioItem item(const char *path, uint64_t token) {
  return (AudioItem){ .token = token, .path = path };
}

static bool contiguous(size_t from, size_t n, int id, int64_t first) {
  pthread_mutex_lock(&F.mu);
  bool ok = from + n <= F.nrec;
  for (size_t i = 0; ok && i < n; i++) {
    ok = F.rec[from + i] == value(id, first + (int64_t)i);
  }
  pthread_mutex_unlock(&F.mu);
  return ok;
}

static void test_gapless(void) {
  fake_reset();
  AudioItem a = item("a", 1), b = item("b", 2);
  audio_play(&a, 0, false);
  CHECK(expect(AEV_STARTED, 1));
  CHECK(expect(AEV_NEED_NEXT, 1));
  audio_next(&b, 1);
  CHECK(expect(AEV_STARTED, 2));
  CHECK(expect(AEV_NEED_NEXT, 2));
  audio_next(NULL, 2);
  CHECK(expect(AEV_STOPPED, 2));
  CHECK(recorded() == 2000);
  CHECK(contiguous(0, 1000, 0, 0) && contiguous(1000, 1000, 1, 0));
  CHECK(F.opens == 1 && F.drains == 1);
}

static void test_trim(void) {
  fake_reset();
  AudioItem cue = { .token = 3, .path = "a", .begin_ms = 200, .end_ms = 500 };
  audio_play(&cue, 50, false);
  CHECK(expect(AEV_STARTED, 3));
  CHECK(expect(AEV_NEED_NEXT, 3));
  audio_next(NULL, 3);
  CHECK(expect(AEV_STOPPED, 3));
  CHECK(recorded() == 250 && contiguous(0, 250, 0, 250));
}

static void test_format_change(void) {
  fake_reset();
  AudioItem a = item("a", 4), c = item("c16", 5);
  audio_play(&a, 0, false);
  CHECK(expect(AEV_STARTED, 4));
  CHECK(expect(AEV_NEED_NEXT, 4));
  audio_next(&c, 4);
  CHECK(expect(AEV_STARTED, 5));
  CHECK(expect(AEV_NEED_NEXT, 5));
  audio_next(NULL, 5);
  CHECK(expect(AEV_STOPPED, 5));
  CHECK(F.opens == 2 && F.drains == 2);
  CHECK(recorded() == 1500 && contiguous(1000, 500, 2, 0));
}

static void pause_resume(bool hw) {
  fake_reset();
  F.can_pause = hw;
  F.max_chunk = 10;
  F.sleep_us = 1000;
  AudioItem l = item("long", 6);
  audio_play(&l, 0, false);
  CHECK(expect(AEV_STARTED, 6));
  usleep(100 * 1000);
  audio_pause(true);
  usleep(50 * 1000);
  size_t at_pause = recorded();
  usleep(100 * 1000);
  CHECK(recorded() == at_pause && at_pause > 0);
  CHECK(audio_position_ms() == at_pause);
  audio_pause(false);
  usleep(100 * 1000);
  CHECK(recorded() > at_pause);
  audio_stop();
  usleep(50 * 1000);
  size_t n = recorded();
  CHECK(contiguous(0, n, 3, 0));
  CHECK(F.opens == 1 && F.decoder_opens == (hw ? 1 : 2));
}

static void test_pause(void) {
  pause_resume(true);
  pause_resume(false);
}

static void test_errors(void) {
  fake_reset();
  AudioItem missing = item("missing", 7);
  audio_play(&missing, 0, false);
  CHECK(expect(AEV_ERROR, 7));
  audio_stop();

  fake_reset();
  F.fail = true;
  AudioItem a = item("a", 8);
  audio_play(&a, 0, false);
  AudioEvent ev;
  bool got = false;
  while (!got && next_event(&ev)) {
    got = ev.type == AEV_OUTPUT_ERROR && ev.token == 8;
  }
  CHECK(got);
}

int main(void) {
  CHECK(audio_init("fake", &event_fd));
  test_gapless();
  test_trim();
  test_format_change();
  test_pause();
  test_errors();
  audio_shutdown();
  fake_reset();
  if (failures) {
    fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  printf("all engine tests passed\n");
  return 0;
}
