#include "store/store.h"
#include "util/util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_MODULE "store"
#define PLAYLIST_MAGIC "mpzd-playlist 1"
#define STATE_MAGIC "mpzd-state 1"

static char *store_dir;

void store_escape(StrBuf *sb, const char *s) {
  const char *run = s;
  for (; *s; s++) {
    const char *rep = NULL;
    switch (*s) {
      case '\\': rep = "\\\\"; break;
      case '\t': rep = "\\t"; break;
      case '\n': rep = "\\n"; break;
      case '\r': rep = "\\r"; break;
    }
    if (rep) {
      sb_append(sb, run, (size_t)(s - run));
      sb_puts(sb, rep);
      run = s + 1;
    }
  }
  sb_append(sb, run, (size_t)(s - run));
}

void store_unescape(char *s) {
  char *w = s;
  for (; *s; s++) {
    if (*s == '\\' && s[1]) {
      s++;
      switch (*s) {
        case 't': *w++ = '\t'; break;
        case 'n': *w++ = '\n'; break;
        case 'r': *w++ = '\r'; break;
        default: *w++ = *s; break;
      }
    } else {
      *w++ = *s;
    }
  }
  *w = '\0';
}

static int split_tabs(char *line, char **fields, int max) {
  int n = 0;
  fields[n++] = line;
  for (char *p = line; *p && n < max; p++) {
    if (*p == '\t') {
      *p = '\0';
      fields[n++] = p + 1;
    }
  }
  for (int i = 0; i < n; i++) {
    store_unescape(fields[i]);
  }
  return n;
}

static void chomp(char *s) {
  size_t n = strlen(s);
  while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
    s[--n] = '\0';
  }
}

static uint32_t u32_or(const char *s, uint32_t def) {
  uint32_t v;
  return parse_u32(s, &v) ? v : def;
}

bool store_write_playlist(FILE *f, const Playlist *p) {
  StrBuf sb = { 0 };
  sb_printf(&sb, PLAYLIST_MAGIC "\n%u\t", p->id);
  store_escape(&sb, p->name);
  sb_printf(&sb, "\t%s\t%u\n", order_name(p->order), p->next_track_id);
  for (size_t i = 0; i < p->len; i++) {
    const Track *t = &p->tracks[i];
    sb_printf(&sb, "%u\t", t->id);
    store_escape(&sb, track_str(t, TS_PATH));
    sb_printf(&sb, "\t%u\t%u\t%u", t->begin_ms, t->end_ms, t->dur_ms);
    for (int s = TS_ARTIST; s < TS_COUNT; s++) {
      sb_puts(&sb, "\t");
      store_escape(&sb, track_str(t, s));
    }
    sb_printf(&sb, "\t%u\t%u\t%u\t%u\n", t->year, t->number, t->disc, t->flags);
    if (sb.len > 64 * 1024) {
      fwrite(sb.data, 1, sb.len, f);
      sb_reset(&sb);
    }
  }
  if (sb.len) {
    fwrite(sb.data, 1, sb.len, f);
  }
  sb_free(&sb);
  return !ferror(f);
}

Playlist *store_read_playlist(FILE *f) {
  char *line = NULL;
  size_t cap = 0;
  Playlist *p = NULL;
  char *fields[16];

  if (getline(&line, &cap, f) < 0) {
    goto out;
  }
  chomp(line);
  if (strcmp(line, PLAYLIST_MAGIC) != 0 || getline(&line, &cap, f) < 0) {
    goto out;
  }
  chomp(line);
  if (split_tabs(line, fields, 16) < 4) {
    goto out;
  }
  p = playlist_new(u32_or(fields[0], 0), fields[1]);
  if (!order_parse(fields[2], &p->order)) {
    p->order = ORDER_INHERIT;
  }
  p->next_track_id = u32_or(fields[3], 1);

  while (getline(&line, &cap, f) >= 0) {
    chomp(line);
    if (split_tabs(line, fields, 16) < 13) {
      continue;
    }
    TrackInfo info = {
      .str = {
        [TS_PATH] = fields[1],
        [TS_ARTIST] = fields[5],
        [TS_ALBUM_ARTIST] = fields[6],
        [TS_ALBUM] = fields[7],
        [TS_TITLE] = fields[8],
      },
      .begin_ms = u32_or(fields[2], 0),
      .end_ms = u32_or(fields[3], 0),
      .dur_ms = u32_or(fields[4], 0),
      .year = (uint16_t)u32_or(fields[9], 0),
      .number = (uint16_t)u32_or(fields[10], 0),
      .disc = (uint16_t)u32_or(fields[11], 0),
      .flags = (uint8_t)u32_or(fields[12], 0),
    };
    Track t;
    track_init(&t, &info);
    t.id = u32_or(fields[0], 0);
    if (t.id >= p->next_track_id) {
      p->next_track_id = t.id + 1;
    }
    VEC_PUSH(p->tracks, p->len, p->cap, t);
  }

out:
  free(line);
  return p;
}

static char *path_in_store(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static char *path_in_store(const char *fmt, ...) {
  StrBuf sb = { 0 };
  sb_puts(&sb, store_dir);
  sb_puts(&sb, "/");
  va_list ap;
  va_start(ap, fmt);
  sb_vprintf(&sb, fmt, ap);
  va_end(ap);
  return sb.data;
}

static int mkdir_p(const char *path) {
  char *tmp = xstrdup(path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        free(tmp);
        return -1;
      }
      *p = '/';
    }
  }
  int rc = (mkdir(tmp, 0755) != 0 && errno != EEXIST) ? -1 : 0;
  free(tmp);
  return rc;
}

bool store_init(const char *dir) {
  store_dir = xstrdup(dir);
  char *pl_dir = path_in_store("playlists");
  int rc = mkdir_p(pl_dir);
  free(pl_dir);
  if (rc != 0) {
    LOG("cannot create %s: %s", dir, strerror(errno));
    return false;
  }
  return true;
}

void store_free(void) {
  free(store_dir);
  store_dir = NULL;
}

static bool atomic_write(const char *path, bool (*writer)(FILE *, const void *), const void *ctx) {
  StrBuf tmp = { 0 };
  sb_printf(&tmp, "%s.tmp", path);
  FILE *f = fopen(tmp.data, "w");
  if (!f) {
    LOG("cannot write %s: %s", tmp.data, strerror(errno));
    sb_free(&tmp);
    return false;
  }
  bool ok = writer(f, ctx) && fflush(f) == 0 && fsync(fileno(f)) == 0;
  ok = (fclose(f) == 0) && ok;
  if (ok && rename(tmp.data, path) != 0) {
    ok = false;
  }
  if (!ok) {
    LOG("failed writing %s: %s", path, strerror(errno));
    unlink(tmp.data);
  }
  sb_free(&tmp);
  return ok;
}

static bool write_playlist_cb(FILE *f, const void *ctx) {
  return store_write_playlist(f, ctx);
}

static bool write_state_cb(FILE *f, const void *ctx) {
  const StorePlaying *playing = ctx;
  StrBuf sb = { 0 };
  sb_printf(&sb, STATE_MAGIC "\nnext_playlist_id %u\norder %s\nfollow %d\nvolume %d\n", M.next_pl_id, order_name(M.order), M.follow ? 1 : 0, M.volume);
  if (M.sel_pl) {
    sb_printf(&sb, "selected %u %u\n", M.sel_pl, M.sel_track);
  }
  if (playing && playing->valid) {
    sb_printf(&sb, "playing %u %u %u\n", playing->pl, playing->track, playing->pos_ms);
  }
  for (size_t i = 0; i < M.len; i++) {
    sb_printf(&sb, "playlist %u\n", M.pls[i]->id);
  }
  bool ok = fwrite(sb.data, 1, sb.len, f) == sb.len;
  sb_free(&sb);
  return ok;
}

bool store_load(StorePlaying *playing) {
  memset(playing, 0, sizeof(*playing));
  char *path = path_in_store("state");
  FILE *f = fopen(path, "r");
  free(path);
  if (!f) {
    return errno == ENOENT;
  }
  char *line = NULL;
  size_t cap = 0;
  bool ok = getline(&line, &cap, f) >= 0;
  if (ok) {
    chomp(line);
    ok = strcmp(line, STATE_MAGIC) == 0;
  }
  while (ok && getline(&line, &cap, f) >= 0) {
    chomp(line);
    char *val = strchr(line, ' ');
    if (!val) {
      continue;
    }
    *val++ = '\0';
    if (strcmp(line, "next_playlist_id") == 0) {
      M.next_pl_id = u32_or(val, 1);
    } else if (strcmp(line, "order") == 0) {
      Order o;
      if (order_parse(val, &o) && o != ORDER_INHERIT) {
        M.order = o;
      }
    } else if (strcmp(line, "follow") == 0) {
      M.follow = atoi(val) != 0;
    } else if (strcmp(line, "volume") == 0) {
      int v = atoi(val);
      M.volume = v < 0 ? 0 : (v > 100 ? 100 : v);
    } else if (strcmp(line, "selected") == 0) {
      sscanf(val, "%u %u", &M.sel_pl, &M.sel_track);
    } else if (strcmp(line, "playing") == 0) {
      playing->valid = sscanf(val, "%u %u %u", &playing->pl, &playing->track, &playing->pos_ms) == 3;
    } else if (strcmp(line, "playlist") == 0) {
      uint32_t id = u32_or(val, 0);
      char *pl_path = path_in_store("playlists/%u.tsv", id);
      FILE *pf = fopen(pl_path, "r");
      if (pf) {
        Playlist *p = store_read_playlist(pf);
        fclose(pf);
        if (p && p->id == id && !model_find(id)) {
          model_add(p);
        } else {
          LOG("skipping corrupt playlist %s", pl_path);
          playlist_free(p);
        }
      } else {
        LOG("missing playlist %s", pl_path);
      }
      free(pl_path);
    }
  }
  free(line);
  fclose(f);
  M.dirty = false;
  M.list_dirty = false;
  if (!ok) {
    LOG("unrecognized state file");
  }
  return ok;
}

bool store_flush(const StorePlaying *playing, bool force_state) {
  bool ok = true;
  for (size_t i = 0; i < M.len; i++) {
    Playlist *p = M.pls[i];
    if (!p->dirty) {
      continue;
    }
    char *path = path_in_store("playlists/%u.tsv", p->id);
    if (atomic_write(path, write_playlist_cb, p)) {
      p->dirty = false;
    } else {
      ok = false;
    }
    free(path);
  }
  if (M.dirty || M.list_dirty || force_state) {
    char *path = path_in_store("state");
    if (atomic_write(path, write_state_cb, playing)) {
      M.dirty = false;
      M.list_dirty = false;
    } else {
      ok = false;
    }
    free(path);
  }
  return ok;
}

void store_playlist_deleted(uint32_t id) {
  char *path = path_in_store("playlists/%u.tsv", id);
  if (unlink(path) != 0 && errno != ENOENT) {
    LOG("cannot remove %s: %s", path, strerror(errno));
  }
  free(path);
}
