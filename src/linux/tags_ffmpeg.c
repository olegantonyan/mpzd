#include "library/tags.h"
#include "util/util.h"

#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <stdlib.h>
#include <string.h>

static const char *meta(const AVFormatContext *fmt, const AVStream *st, const char *key) {
  const AVDictionaryEntry *e = av_dict_get(fmt->metadata, key, NULL, 0);
  if (!e && st) {
    e = av_dict_get(st->metadata, key, NULL, 0);
  }
  return e ? e->value : NULL;
}

static uint16_t meta_num(const AVFormatContext *fmt, const AVStream *st, const char *key) {
  const char *v = meta(fmt, st, key);
  if (!v) {
    return 0;
  }
  long n = strtol(v, NULL, 10);
  return (n > 0 && n <= UINT16_MAX) ? (uint16_t)n : 0;
}

static AVFormatContext *open_probe(const char *path, int *stream_index, uint32_t *dur_ms) {
  AVFormatContext *fmt = NULL;
  if (avformat_open_input(&fmt, path, NULL, NULL) < 0) {
    return NULL;
  }
  int si = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  const AVStream *st = si >= 0 ? fmt->streams[si] : NULL;
  int64_t dur = AV_NOPTS_VALUE;
  if (st && st->duration != AV_NOPTS_VALUE && st->duration > 0) {
    dur = av_rescale_q(st->duration, st->time_base, (AVRational){ 1, 1000 });
  }
  if (si < 0 || dur == AV_NOPTS_VALUE) {
    if (avformat_find_stream_info(fmt, NULL) < 0) {
      avformat_close_input(&fmt);
      return NULL;
    }
    si = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (si < 0) {
      avformat_close_input(&fmt);
      return NULL;
    }
    st = fmt->streams[si];
    if (st->duration != AV_NOPTS_VALUE && st->duration > 0) {
      dur = av_rescale_q(st->duration, st->time_base, (AVRational){ 1, 1000 });
    } else if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) {
      dur = fmt->duration / 1000;
    }
  }
  *stream_index = si;
  *dur_ms = (dur > 0 && dur < UINT32_MAX) ? (uint32_t)dur : 0;
  return fmt;
}

bool tags_read(const char *path, Track *out) {
  int si;
  uint32_t dur_ms;
  AVFormatContext *fmt = open_probe(path, &si, &dur_ms);
  if (!fmt) {
    return false;
  }
  const AVStream *st = fmt->streams[si];

  const char *artist = meta(fmt, st, "artist");
  const char *album_artist = meta(fmt, st, "album_artist");
  TrackInfo info = {
    .str = {
      [TS_PATH] = path,
      [TS_ARTIST] = artist ? artist : album_artist,
      [TS_ALBUM_ARTIST] = album_artist,
      [TS_ALBUM] = meta(fmt, st, "album"),
      [TS_TITLE] = meta(fmt, st, "title"),
    },
    .dur_ms = dur_ms,
    .year = meta_num(fmt, st, "date"),
    .number = meta_num(fmt, st, "track"),
    .disc = meta_num(fmt, st, "disc"),
  };
  if (!info.year) {
    info.year = meta_num(fmt, st, "year");
  }
  track_init(out, &info);
  avformat_close_input(&fmt);
  return true;
}

uint32_t tags_duration_ms(const char *path) {
  int si;
  uint32_t dur_ms = 0;
  AVFormatContext *fmt = open_probe(path, &si, &dur_ms);
  if (fmt) {
    avformat_close_input(&fmt);
  }
  return dur_ms;
}
