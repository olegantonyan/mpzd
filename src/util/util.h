#ifndef UTIL_UTIL_H
#define UTIL_UTIL_H

#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define LOG(fmt, ...) fprintf(stderr, "[" LOG_MODULE "] " fmt "\n", ##__VA_ARGS__)

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define VEC_RESERVE(ptr, len, cap, extra) do { \
  if ((len) + (extra) > (cap)) { \
    size_t vec_newcap__ = (cap) ? (cap) * 2 : 16; \
    while (vec_newcap__ < (len) + (extra)) vec_newcap__ *= 2; \
    (ptr) = xrealloc((ptr), vec_newcap__ * sizeof(*(ptr))); \
    (cap) = vec_newcap__; \
  } \
} while (0)

#define VEC_PUSH(ptr, len, cap, item) do { \
  VEC_RESERVE(ptr, len, cap, 1); \
  (ptr)[(len)++] = (item); \
} while (0)

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} StrBuf;

void sb_append(StrBuf *sb, const char *s, size_t n);
void sb_puts(StrBuf *sb, const char *s);
void sb_printf(StrBuf *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_vprintf(StrBuf *sb, const char *fmt, va_list ap);
void sb_consume(StrBuf *sb, size_t n);
void sb_reset(StrBuf *sb);
void sb_free(StrBuf *sb);

typedef struct {
  char **v;
  size_t len;
  size_t cap;
} StrVec;

void strvec_push(StrVec *sv, char *s);
void strvec_free(StrVec *sv);

bool parse_u32(const char *s, uint32_t *out);
bool parse_i64(const char *s, int64_t *out);

bool path_is_url(const char *s);
const char *path_basename(const char *path);
size_t path_dirname_len(const char *path);
const char *path_ext(const char *path);
bool ext_in(const char *path, const char *const *list);
bool is_audio_file(const char *path);
bool is_cue_file(const char *path);
bool is_playlist_file(const char *path);
bool is_listable_file(const char *path);

uint32_t rng_u32(void);
uint64_t mono_ms(void);

bool join_within(pthread_t t, pthread_mutex_t *mu, pthread_cond_t *cv, const bool *done, unsigned sec);

#endif
