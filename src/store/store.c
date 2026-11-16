#include "store/store.h"
#include "util/util.h"

#include <inttypes.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#define LOG_MODULE "store"
#define PLAYLIST_MAGIC "mpzd-playlist 1"
#define MAX_PLAYLIST_BYTES (256u << 20)
#define STATE_MAGIC "mpzd-state 1"
#define RETRY_SEC 10

typedef struct WItem {
  char *path;
  char *data;
  size_t len;
  bool gz;
  char *also_unlink;
  struct WItem *next;
} WItem;

static struct {
  pthread_t thread;
  pthread_mutex_t mu;
  pthread_cond_t cv;
  pthread_cond_t idle;
  WItem *head;
  WItem *tail;
  WItem *busy;
  bool superseded;
  bool quit;
  bool running;
} W = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .idle = PTHREAD_COND_INITIALIZER };

static char *store_dir;

static void escape(StrBuf *sb, const char *s) {
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

static void unescape(char *s) {
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
    unescape(fields[i]);
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

static bool write_tsv(FILE *f, const Playlist *p) {
  StrBuf sb = { 0 };
  sb_printf(&sb, PLAYLIST_MAGIC "\n%" PRIu32 "\t", p->id);
  escape(&sb, p->name);
  sb_printf(&sb, "\t%s\t%" PRIu32 "\n", order_name(p->order), p->next_track_id);
  for (size_t i = 0; i < p->len; i++) {
    const Track *t = &p->tracks[i];
    sb_printf(&sb, "%" PRIu32 "\t", t->id);
    escape(&sb, track_str(t, TS_PATH));
    sb_printf(&sb, "\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32, t->begin_ms, t->end_ms, t->dur_ms);
    for (int s = TS_ARTIST; s < TS_COUNT; s++) {
      sb_puts(&sb, "\t");
      escape(&sb, track_str(t, s));
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

static Playlist *read_tsv(FILE *f) {
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

static bool gzip(const char *src, size_t len, StrBuf *out) {
  z_stream zs = { 0 };
  if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 9, Z_DEFAULT_STRATEGY) != Z_OK) {
    return false;
  }
  out->cap = deflateBound(&zs, len);
  out->data = xrealloc(out->data, out->cap);
  zs.next_in = (Bytef *)src;
  zs.avail_in = (uInt)len;
  zs.next_out = (Bytef *)out->data;
  zs.avail_out = (uInt)out->cap;
  int rc = deflate(&zs, Z_FINISH);
  out->len = zs.total_out;
  deflateEnd(&zs);
  return rc == Z_STREAM_END;
}

static bool gunzip(const uint8_t *src, size_t len, StrBuf *out) {
  z_stream zs = { 0 };
  if (inflateInit2(&zs, 15 + 16) != Z_OK) {
    return false;
  }
  zs.next_in = (Bytef *)src;
  zs.avail_in = (uInt)len;
  int rc = Z_OK;
  while (rc == Z_OK) {
    if (out->len == out->cap) {
      if (out->cap >= MAX_PLAYLIST_BYTES) {
        break;
      }
      out->cap = out->cap ? out->cap * 2 : 64 * 1024;
      out->data = xrealloc(out->data, out->cap);
    }
    zs.next_out = (Bytef *)out->data + out->len;
    zs.avail_out = (uInt)(out->cap - out->len);
    rc = inflate(&zs, Z_NO_FLUSH);
    out->len = zs.total_out;
  }
  inflateEnd(&zs);
  return rc == Z_STREAM_END && zs.avail_in == 0;
}

bool store_write_playlist(FILE *f, const Playlist *p) {
  char *tsv = NULL;
  size_t len = 0;
  FILE *mem = open_memstream(&tsv, &len);
  if (!mem) {
    return false;
  }
  bool ok = write_tsv(mem, p);
  ok = fclose(mem) == 0 && ok;
  StrBuf gz = { 0 };
  ok = ok && gzip(tsv, len, &gz) && fwrite(gz.data, 1, gz.len, f) == gz.len;
  free(tsv);
  sb_free(&gz);
  return ok && !ferror(f);
}

Playlist *store_read_playlist(FILE *f) {
  StrBuf raw = { 0 };
  char chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
    sb_append(&raw, chunk, n);
  }
  Playlist *p = NULL;
  const uint8_t *b = (const uint8_t *)raw.data;
  bool gz = raw.len >= 2 && b[0] == 0x1f && b[1] == 0x8b;
  StrBuf tsv = { 0 };
  if (!ferror(f) && raw.len && (!gz || gunzip(b, raw.len, &tsv))) {
    StrBuf *src = gz ? &tsv : &raw;
    FILE *mem = fmemopen(src->data, src->len, "r");
    if (mem) {
      p = read_tsv(mem);
      fclose(mem);
    }
  }
  sb_free(&raw);
  sb_free(&tsv);
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

static void *writer(void *arg);

bool store_init(const char *dir) {
  store_dir = xstrdup(dir);
  char *pl_dir = path_in_store("playlists");
  int rc = mkdir_p(pl_dir);
  free(pl_dir);
  if (rc != 0) {
    LOG("cannot create %s: %s", dir, strerror(errno));
    return false;
  }
  W.quit = false;
  if (pthread_create(&W.thread, NULL, writer, NULL) != 0) {
    LOG("cannot start writer thread");
    return false;
  }
  W.running = true;
  return true;
}

void store_free(void) {
  free(store_dir);
  store_dir = NULL;
}

static bool atomic_write(const char *path, const char *data, size_t len) {
  StrBuf tmp = { 0 };
  sb_printf(&tmp, "%s.tmp", path);
  FILE *f = fopen(tmp.data, "w");
  if (!f) {
    LOG("cannot write %s: %s", tmp.data, strerror(errno));
    sb_free(&tmp);
    return false;
  }
  bool ok = fwrite(data, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
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

static void unlink_logged(const char *path) {
  if (unlink(path) != 0 && errno != ENOENT) {
    LOG("cannot remove %s: %s", path, strerror(errno));
  }
}

static bool process(WItem *it) {
  if (!it->data) {
    unlink_logged(it->path);
    return true;
  }
  StrBuf gz = { 0 };
  if (it->gz && !gzip(it->data, it->len, &gz)) {
    LOG("cannot compress %s", it->path);
    return true;
  }
  bool ok = it->gz ? atomic_write(it->path, gz.data, gz.len) : atomic_write(it->path, it->data, it->len);
  sb_free(&gz);
  if (ok && it->also_unlink) {
    unlink_logged(it->also_unlink);
  }
  return ok;
}

static void witem_free(WItem *it) {
  free(it->path);
  free(it->data);
  free(it->also_unlink);
  free(it);
}

static void *writer(void *arg) {
  (void)arg;
  pthread_mutex_lock(&W.mu);
  for (;;) {
    while (!W.head && !W.quit) {
      pthread_cond_wait(&W.cv, &W.mu);
    }
    if (!W.head) {
      break;
    }
    WItem *it = W.head;
    W.head = it->next;
    if (!W.head) {
      W.tail = NULL;
    }
    W.busy = it;
    W.superseded = false;
    pthread_mutex_unlock(&W.mu);
    bool failing = false;
    for (;;) {
      if (process(it)) {
        if (failing) {
          LOG("saved %s", it->path);
        }
        break;
      }
      if (!failing) {
        LOG("retrying every %d s", RETRY_SEC);
        failing = true;
      }
      pthread_mutex_lock(&W.mu);
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += RETRY_SEC;
      while (!W.superseded && !W.quit && pthread_cond_timedwait(&W.cv, &W.mu, &ts) == 0) {
      }
      bool stop = W.superseded || W.quit;
      pthread_mutex_unlock(&W.mu);
      if (stop) {
        break;
      }
    }
    witem_free(it);
    pthread_mutex_lock(&W.mu);
    W.busy = NULL;
    pthread_cond_broadcast(&W.idle);
  }
  pthread_mutex_unlock(&W.mu);
  return NULL;
}

static void enqueue(char *path, char *data, size_t len, bool gz, char *also_unlink) {
  WItem *it = xcalloc(1, sizeof(*it));
  it->path = path;
  it->data = data;
  it->len = len;
  it->gz = gz;
  it->also_unlink = also_unlink;
  pthread_mutex_lock(&W.mu);
  WItem **pp = &W.head;
  W.tail = NULL;
  while (*pp) {
    WItem *q = *pp;
    if (strcmp(q->path, path) == 0) {
      *pp = q->next;
      witem_free(q);
    } else {
      W.tail = q;
      pp = &q->next;
    }
  }
  if (W.busy && strcmp(W.busy->path, path) == 0) {
    W.superseded = true;
  }
  *pp = it;
  W.tail = it;
  pthread_cond_broadcast(&W.cv);
  pthread_mutex_unlock(&W.mu);
}

static char *state_text(const StorePlaying *playing, size_t *len) {
  StrBuf sb = { 0 };
  sb_printf(&sb, STATE_MAGIC "\nnext_playlist_id %" PRIu32 "\norder %s\nfollow %d\nvolume %d\n", M.next_pl_id, order_name(M.order), M.follow ? 1 : 0, M.volume);
  if (M.output) {
    sb_printf(&sb, "output %s\n", M.output);
  }
  if (M.sel_pl) {
    sb_printf(&sb, "selected %" PRIu32 " %" PRIu32 "\n", M.sel_pl, M.sel_track);
  }
  if (playing && playing->valid) {
    sb_printf(&sb, "playing %" PRIu32 " %" PRIu32 " %" PRIu32 "\n", playing->pl, playing->track, playing->pos_ms);
  }
  for (size_t i = 0; i < M.len; i++) {
    sb_printf(&sb, "playlist %" PRIu32 "\n", M.pls[i]->id);
  }
  *len = sb.len;
  return sb.data;
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
    } else if (strcmp(line, "output") == 0) {
      free(M.output);
      M.output = xstrdup(val);
    } else if (strcmp(line, "selected") == 0) {
      sscanf(val, "%" SCNu32 " %" SCNu32, &M.sel_pl, &M.sel_track);
    } else if (strcmp(line, "playing") == 0) {
      playing->valid = sscanf(val, "%" SCNu32 " %" SCNu32 " %" SCNu32, &playing->pl, &playing->track, &playing->pos_ms) == 3;
    } else if (strcmp(line, "playlist") == 0) {
      uint32_t id = u32_or(val, 0);
      char *pl_path = path_in_store("playlists/%" PRIu32 ".tsv.gz", id);
      FILE *pf = fopen(pl_path, "r");
      bool legacy = false;
      if (!pf) {
        free(pl_path);
        pl_path = path_in_store("playlists/%" PRIu32 ".tsv", id);
        pf = fopen(pl_path, "r");
        legacy = pf != NULL;
      }
      if (pf) {
        Playlist *p = store_read_playlist(pf);
        fclose(pf);
        if (p && p->id == id && !model_find(id)) {
          p->dirty = legacy;
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
    char *tsv = NULL;
    size_t len = 0;
    FILE *mem = open_memstream(&tsv, &len);
    bool written = mem && write_tsv(mem, p);
    written = mem && fclose(mem) == 0 && written;
    if (!written) {
      LOG("cannot serialize playlist %" PRIu32, p->id);
      free(tsv);
      ok = false;
      continue;
    }
    enqueue(path_in_store("playlists/%" PRIu32 ".tsv.gz", p->id), tsv, len, true, path_in_store("playlists/%" PRIu32 ".tsv", p->id));
    p->dirty = false;
  }
  if (M.dirty || M.list_dirty || force_state) {
    size_t len;
    char *text = state_text(playing, &len);
    enqueue(path_in_store("state"), text, len, false, NULL);
    M.dirty = false;
    M.list_dirty = false;
  }
  return ok;
}

void store_playlist_deleted(uint32_t id) {
  enqueue(path_in_store("playlists/%" PRIu32 ".tsv.gz", id), NULL, 0, false, NULL);
  enqueue(path_in_store("playlists/%" PRIu32 ".tsv", id), NULL, 0, false, NULL);
}

bool store_shutdown(unsigned timeout_ms) {
  if (!W.running) {
    return true;
  }
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += timeout_ms / 1000;
  ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000L;
  }
  pthread_mutex_lock(&W.mu);
  while ((W.head || W.busy) && pthread_cond_timedwait(&W.idle, &W.mu, &ts) == 0) {
  }
  bool drained = !W.head && !W.busy;
  W.quit = true;
  pthread_cond_broadcast(&W.cv);
  pthread_mutex_unlock(&W.mu);
  W.running = false;
  if (!drained) {
    LOG("state not saved: disk timed out");
    pthread_detach(W.thread);
    return false;
  }
  pthread_join(W.thread, NULL);
  return true;
}
