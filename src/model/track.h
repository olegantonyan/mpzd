#ifndef MODEL_TRACK_H
#define MODEL_TRACK_H

#include <stddef.h>
#include <stdint.h>

enum { TS_PATH, TS_ARTIST, TS_ALBUM_ARTIST, TS_ALBUM, TS_TITLE, TS_COUNT };

enum { TRACK_CUE = 1, TRACK_STREAM = 2 };

typedef struct {
  const char *str[TS_COUNT];
  uint32_t begin_ms;
  uint32_t end_ms;
  uint32_t dur_ms;
  uint16_t year;
  uint16_t number;
  uint16_t disc;
  uint8_t flags;
} TrackInfo;

typedef struct {
  char *s;
  uint32_t id;
  uint32_t begin_ms;
  uint32_t end_ms;
  uint32_t dur_ms;
  uint16_t off[TS_COUNT];
  uint16_t year;
  uint16_t number;
  uint16_t disc;
  uint8_t flags;
} Track;

typedef struct {
  Track *v;
  size_t len;
  size_t cap;
} TrackVec;

void track_init(Track *t, const TrackInfo *info);
void track_copy(Track *dst, const Track *src);
void track_free(Track *t);
const char *track_str(const Track *t, int which);
void track_info(const Track *t, TrackInfo *out);
const char *track_display_title(const Track *t);

void trackvec_push(TrackVec *tv, Track *t);
void trackvec_append(TrackVec *dst, TrackVec *src);
void trackvec_free(TrackVec *tv);

#endif
