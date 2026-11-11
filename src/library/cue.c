#include "library/cue.h"
#include "util/util.h"

#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define CUE_MAX_SIZE (1024 * 1024)

typedef struct {
  int file;
  int number;
  char *performer;
  char *title;
  int64_t idx0;
  int64_t idx1;
} CueEntry;

static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  char *buf = xmalloc(CUE_MAX_SIZE + 1);
  size_t n = fread(buf, 1, CUE_MAX_SIZE, f);
  fclose(f);
  buf[n] = '\0';
  return buf;
}

static char *skip_ws(char *s) {
  while (*s == ' ' || *s == '\t') s++;
  return s;
}

static char *take_word(char **s) {
  char *p = skip_ws(*s);
  char *w = p;
  while (*p && *p != ' ' && *p != '\t') p++;
  if (*p) {
    *p++ = '\0';
  }
  *s = p;
  return w;
}

static char *take_value(char **s) {
  char *p = skip_ws(*s);
  if (*p == '"') {
    char *v = ++p;
    char *q = strrchr(p, '"');
    if (q) {
      *q = '\0';
      *s = q + 1;
    } else {
      *s = p + strlen(p);
    }
    return v;
  }
  return take_word(s);
}

static int64_t parse_msf(const char *s) {
  int m = 0, sec = 0, fr = 0;
  if (sscanf(s, "%d:%d:%d", &m, &sec, &fr) != 3) {
    return -1;
  }
  return ((int64_t)m * 60 + sec) * 1000 + (int64_t)fr * 1000 / 75;
}

static bool file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static char *resolve_file(const char *dir, size_t dirlen, const char *name) {
  const char *bs = strrchr(name, '\\');
  if (bs) {
    name = bs + 1;
  }
  StrBuf p = { 0 };
  if (name[0] == '/') {
    sb_puts(&p, name);
  } else {
    sb_append(&p, dir, dirlen);
    sb_puts(&p, "/");
    sb_puts(&p, name);
  }
  if (file_exists(p.data)) {
    return p.data;
  }

  char *dirpath = xstrndup(p.data, path_dirname_len(p.data));
  const char *want = path_basename(p.data);
  const char *want_ext = path_ext(want);
  size_t want_stem = *want_ext ? (size_t)(want_ext - want - 1) : strlen(want);
  DIR *d = opendir(dirpath);
  char *found = NULL;
  char *stem_match = NULL;
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      if (strcasecmp(e->d_name, want) == 0) {
        found = xstrdup(e->d_name);
        break;
      }
      if (!stem_match && is_audio_file(e->d_name) && strncasecmp(e->d_name, want, want_stem) == 0 && e->d_name[want_stem] == '.') {
        stem_match = xstrdup(e->d_name);
      }
    }
    closedir(d);
  }
  char *result = NULL;
  const char *pick = found ? found : stem_match;
  if (pick) {
    StrBuf r = { 0 };
    sb_puts(&r, dirpath);
    sb_puts(&r, "/");
    sb_puts(&r, pick);
    result = r.data;
  }
  free(found);
  free(stem_match);
  free(dirpath);
  sb_free(&p);
  return result;
}

bool cue_parse(const char *cue_path, CueDurationFn duration, TrackVec *out, StrVec *files) {
  char *buf = read_file(cue_path);
  if (!buf) {
    return false;
  }
  char *text = buf;
  if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
    text += 3;
  }

  size_t dirlen = path_dirname_len(cue_path);
  size_t files_base = files->len;
  char *album_performer = NULL;
  char *album_title = NULL;
  uint16_t year = 0;
  uint16_t disc = 0;
  CueEntry *entries = NULL;
  size_t n = 0, cap = 0;
  int cur_file = -1;

  char *save = NULL;
  for (char *line = strtok_r(text, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
    char *s = line;
    char *cmd = take_word(&s);
    CueEntry *cur = n ? &entries[n - 1] : NULL;
    if (strcasecmp(cmd, "REM") == 0) {
      char *key = take_word(&s);
      char *val = take_value(&s);
      if (strcasecmp(key, "DATE") == 0) {
        year = (uint16_t)atoi(val);
      } else if (strcasecmp(key, "DISCNUMBER") == 0) {
        disc = (uint16_t)atoi(val);
      }
    } else if (strcasecmp(cmd, "PERFORMER") == 0) {
      char *v = take_value(&s);
      if (n) {
        cur->performer = v;
      } else {
        album_performer = v;
      }
    } else if (strcasecmp(cmd, "TITLE") == 0) {
      char *v = take_value(&s);
      if (n) {
        cur->title = v;
      } else {
        album_title = v;
      }
    } else if (strcasecmp(cmd, "FILE") == 0) {
      char *resolved = resolve_file(cue_path, dirlen, take_value(&s));
      if (resolved) {
        strvec_push(files, resolved);
        cur_file = (int)(files->len - 1);
      } else {
        cur_file = -1;
      }
    } else if (strcasecmp(cmd, "TRACK") == 0) {
      CueEntry e = { .file = cur_file, .number = atoi(take_word(&s)), .idx0 = -1, .idx1 = -1 };
      VEC_PUSH(entries, n, cap, e);
    } else if (strcasecmp(cmd, "INDEX") == 0 && cur) {
      int idx = atoi(take_word(&s));
      int64_t ms = parse_msf(take_word(&s));
      if (idx == 0) {
        cur->idx0 = ms;
      } else if (idx == 1) {
        cur->idx1 = ms;
      }
    }
  }

  uint32_t file_dur = 0;
  int file_dur_for = -1;
  for (size_t i = 0; i < n; i++) {
    CueEntry *e = &entries[i];
    int64_t begin = e->idx1 >= 0 ? e->idx1 : e->idx0;
    if (e->file < 0 || begin < 0) {
      continue;
    }
    const char *path = files->v[e->file];
    int64_t end = 0;
    for (size_t j = i + 1; j < n; j++) {
      if (entries[j].file != e->file) {
        break;
      }
      int64_t nb = entries[j].idx1 >= 0 ? entries[j].idx1 : entries[j].idx0;
      if (nb > begin) {
        end = nb;
        break;
      }
    }
    int64_t dur = 0;
    if (end) {
      dur = end - begin;
    } else {
      if (file_dur_for != e->file) {
        file_dur = duration ? duration(path) : 0;
        file_dur_for = e->file;
      }
      dur = file_dur > begin ? file_dur - begin : 0;
    }
    TrackInfo info = {
      .str = {
        [TS_PATH] = path,
        [TS_ARTIST] = e->performer ? e->performer : album_performer,
        [TS_ALBUM_ARTIST] = album_performer,
        [TS_ALBUM] = album_title,
        [TS_TITLE] = e->title,
      },
      .begin_ms = (uint32_t)begin,
      .end_ms = (uint32_t)end,
      .dur_ms = (uint32_t)dur,
      .year = year,
      .number = (uint16_t)e->number,
      .disc = disc,
      .flags = TRACK_CUE,
    };
    Track t;
    track_init(&t, &info);
    trackvec_push(out, &t);
  }

  bool ok = files->len > files_base;
  free(entries);
  free(buf);
  return ok;
}
