#include "util/util.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <time.h>

void *xmalloc(size_t n) {
  void *p = malloc(n ? n : 1);
  if (!p) {
    abort();
  }
  return p;
}

void *xcalloc(size_t n, size_t size) {
  void *p = calloc(n ? n : 1, size ? size : 1);
  if (!p) {
    abort();
  }
  return p;
}

void *xrealloc(void *p, size_t n) {
  p = realloc(p, n ? n : 1);
  if (!p) {
    abort();
  }
  return p;
}

char *xstrdup(const char *s) {
  return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n) {
  char *p = xmalloc(n + 1);
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

void sb_append(StrBuf *sb, const char *s, size_t n) {
  if (sb->len + n + 1 > sb->cap) {
    size_t cap = sb->cap ? sb->cap : 256;
    while (cap < sb->len + n + 1) {
      cap *= 2;
    }
    sb->data = xrealloc(sb->data, cap);
    sb->cap = cap;
  }
  memcpy(sb->data + sb->len, s, n);
  sb->len += n;
  sb->data[sb->len] = '\0';
}

void sb_puts(StrBuf *sb, const char *s) {
  sb_append(sb, s, strlen(s));
}

void sb_vprintf(StrBuf *sb, const char *fmt, va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);
  if (n <= 0) {
    return;
  }
  sb_append(sb, "", 0);
  if (sb->len + (size_t)n + 1 > sb->cap) {
    size_t cap = sb->cap;
    while (cap < sb->len + (size_t)n + 1) {
      cap *= 2;
    }
    sb->data = xrealloc(sb->data, cap);
    sb->cap = cap;
  }
  vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap);
  sb->len += (size_t)n;
}

void sb_printf(StrBuf *sb, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  sb_vprintf(sb, fmt, ap);
  va_end(ap);
}

void sb_consume(StrBuf *sb, size_t n) {
  if (n >= sb->len) {
    sb->len = 0;
  } else {
    memmove(sb->data, sb->data + n, sb->len - n);
    sb->len -= n;
  }
  if (sb->data) {
    sb->data[sb->len] = '\0';
  }
}

void sb_reset(StrBuf *sb) {
  sb->len = 0;
  if (sb->data) {
    sb->data[0] = '\0';
  }
}

void sb_free(StrBuf *sb) {
  free(sb->data);
  sb->data = NULL;
  sb->len = sb->cap = 0;
}

void strvec_push(StrVec *sv, char *s) {
  VEC_PUSH(sv->v, sv->len, sv->cap, s);
}

void strvec_free(StrVec *sv) {
  for (size_t i = 0; i < sv->len; i++) {
    free(sv->v[i]);
  }
  free(sv->v);
  memset(sv, 0, sizeof(*sv));
}

bool parse_u32(const char *s, uint32_t *out) {
  if (!s || !isdigit((unsigned char)*s)) {
    return false;
  }
  errno = 0;
  char *end;
  unsigned long v = strtoul(s, &end, 10);
  if (errno || *end || v > UINT32_MAX) {
    return false;
  }
  *out = (uint32_t)v;
  return true;
}

bool parse_i64(const char *s, int64_t *out) {
  if (!s || !*s) {
    return false;
  }
  errno = 0;
  char *end;
  long long v = strtoll(s, &end, 10);
  if (errno || *end) {
    return false;
  }
  *out = v;
  return true;
}

bool path_is_url(const char *s) {
  return strncasecmp(s, "http://", 7) == 0 || strncasecmp(s, "https://", 8) == 0;
}

const char *path_basename(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

size_t path_dirname_len(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? (size_t)(slash - path) : 0;
}

const char *path_ext(const char *path) {
  const char *base = path_basename(path);
  const char *dot = strrchr(base, '.');
  return (dot && dot != base) ? dot + 1 : "";
}

bool ext_in(const char *path, const char *const *list) {
  const char *ext = path_ext(path);
  for (; *list; list++) {
    if (strcasecmp(ext, *list) == 0) {
      return true;
    }
  }
  return false;
}

static const char *const audio_exts[] = {
  "mp3", "flac", "ogg", "oga", "opus", "spx", "m4a", "m4b", "mp4", "aac", "wav", "aiff", "aif", "aifc",
  "wma", "asf", "ape", "wv", "mpc", "tta", "mka", "dsf", "dff", "shn", NULL
};
static const char *const cue_exts[] = { "cue", NULL };
static const char *const playlist_exts[] = { "m3u", "m3u8", "pls", NULL };

bool is_audio_file(const char *path) {
  return ext_in(path, audio_exts);
}

bool is_cue_file(const char *path) {
  return ext_in(path, cue_exts);
}

bool is_playlist_file(const char *path) {
  return ext_in(path, playlist_exts);
}

bool is_listable_file(const char *path) {
  return is_audio_file(path) || is_cue_file(path) || is_playlist_file(path);
}

uint32_t rng_u32(void) {
  static uint64_t state = 0;
  if (state == 0) {
    if (getrandom(&state, sizeof(state), 0) != sizeof(state) || state == 0) {
      state = 0x9E3779B97F4A7C15ull;
    }
  }
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return (uint32_t)(state >> 32);
}

uint64_t mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
