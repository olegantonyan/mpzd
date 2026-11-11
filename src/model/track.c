#include "model/track.h"
#include "util/util.h"

#include <stdlib.h>
#include <string.h>

#define TAG_MAX 1024
#define PATH_MAX_LEN 8192

void track_init(Track *t, const TrackInfo *info) {
  size_t len[TS_COUNT];
  size_t total = 0;
  for (int i = 0; i < TS_COUNT; i++) {
    const char *s = info->str[i] ? info->str[i] : "";
    size_t max = i == TS_PATH ? PATH_MAX_LEN : TAG_MAX;
    len[i] = strnlen(s, max);
    total += len[i] + 1;
  }

  memset(t, 0, sizeof(*t));
  t->s = xmalloc(total);
  size_t pos = 0;
  for (int i = 0; i < TS_COUNT; i++) {
    const char *s = info->str[i] ? info->str[i] : "";
    t->off[i] = (uint16_t)pos;
    memcpy(t->s + pos, s, len[i]);
    pos += len[i];
    t->s[pos++] = '\0';
  }

  t->begin_ms = info->begin_ms;
  t->end_ms = info->end_ms;
  t->dur_ms = info->dur_ms;
  t->year = info->year;
  t->number = info->number;
  t->disc = info->disc;
  t->flags = info->flags;
}

void track_copy(Track *dst, const Track *src) {
  TrackInfo info;
  track_info(src, &info);
  track_init(dst, &info);
  dst->id = src->id;
}

void track_free(Track *t) {
  free(t->s);
  t->s = NULL;
}

const char *track_str(const Track *t, int which) {
  return t->s + t->off[which];
}

void track_info(const Track *t, TrackInfo *out) {
  for (int i = 0; i < TS_COUNT; i++) {
    out->str[i] = track_str(t, i);
  }
  out->begin_ms = t->begin_ms;
  out->end_ms = t->end_ms;
  out->dur_ms = t->dur_ms;
  out->year = t->year;
  out->number = t->number;
  out->disc = t->disc;
  out->flags = t->flags;
}

const char *track_display_title(const Track *t) {
  const char *title = track_str(t, TS_TITLE);
  if (*title) {
    return title;
  }
  if (t->flags & TRACK_STREAM) {
    return track_str(t, TS_PATH);
  }
  return path_basename(track_str(t, TS_PATH));
}

void trackvec_push(TrackVec *tv, Track *t) {
  VEC_PUSH(tv->v, tv->len, tv->cap, *t);
}

void trackvec_append(TrackVec *dst, TrackVec *src) {
  if (src->len) {
    VEC_RESERVE(dst->v, dst->len, dst->cap, src->len);
    memcpy(dst->v + dst->len, src->v, src->len * sizeof(Track));
    dst->len += src->len;
  }
  free(src->v);
  memset(src, 0, sizeof(*src));
}

void trackvec_free(TrackVec *tv) {
  for (size_t i = 0; i < tv->len; i++) {
    track_free(&tv->v[i]);
  }
  free(tv->v);
  memset(tv, 0, sizeof(*tv));
}
