#include "app.h"
#include "library/library.h"
#include "library/scan.h"
#include "model/model.h"
#include "model/sort.h"
#include "player/player.h"
#include "proto/handlers.h"
#include "proto/proto.h"
#include "store/store.h"
#include "util/util.h"

#include <stdlib.h>
#include <string.h>

#define DEFAULT_PAGE 1000

static void playlist_changed(Playlist *p) {
  server_event("playlist %u", p->id);
  app_changed();
}

static void list_changed(void) {
  server_event("playlists");
  app_changed();
}

static Playlist *arg_playlist(const char *s) {
  uint32_t id;
  if (!arg_u32(s, &id, "playlist id")) {
    return NULL;
  }
  Playlist *p = model_find(id);
  if (!p) {
    r_err(E_NOT_FOUND, "no playlist %u", id);
  }
  return p;
}

static bool arg_ids(int argc, char **argv, uint32_t **out) {
  uint32_t *ids = xmalloc(sizeof(uint32_t) * (size_t)(argc ? argc : 1));
  for (int i = 0; i < argc; i++) {
    if (!arg_u32(argv[i], &ids[i], "track id")) {
      free(ids);
      return false;
    }
  }
  *out = ids;
  return true;
}

static bool arg_at(int *argc, char ***argv, int64_t *at) {
  *at = -1;
  if (*argc >= 2 && strcmp((*argv)[0], "at") == 0) {
    uint32_t pos;
    if (!arg_u32((*argv)[1], &pos, "position")) {
      return false;
    }
    *at = pos;
    *argc -= 2;
    *argv += 2;
  }
  return true;
}

typedef struct {
  Deferred *d;
  char *root;
  StrVec args;
  char *name;
  uint32_t pl;
  int64_t at;
  StrVec paths;
  const char *bad;
} ResolveJob;

static void resolve_work(void *arg) {
  ResolveJob *job = arg;
  for (size_t i = 0; i < job->args.len; i++) {
    const char *a = job->args.v[i];
    char *real = path_is_url(a) ? xstrdup(a) : library_resolve(job->root, a);
    if (!real) {
      job->bad = a;
      return;
    }
    strvec_push(&job->paths, real);
  }
}

static void resolve_free(void *arg) {
  ResolveJob *job = arg;
  free(job->root);
  free(job->name);
  strvec_free(&job->args);
  strvec_free(&job->paths);
  free(job);
}

static bool resolve_async(int argc, char **argv, ResolveJob *job, void (*done)(void *, bool)) {
  job->root = xstrdup(M.library);
  for (int i = 0; i < argc; i++) {
    strvec_push(&job->args, xstrdup(argv[i]));
  }
  return r_async(job, &job->d, resolve_work, done, resolve_free, FS_TIMEOUT_MS);
}

static bool resolved(ResolveJob *job, bool timed_out) {
  if (!r_resume(job->d, timed_out)) {
    return false;
  }
  if (job->bad) {
    return r_err(E_NOT_FOUND, "'%s' not found or outside the library", job->bad);
  }
  return true;
}

static void submit(Playlist *p, ScanKind kind, int64_t at, StrVec *paths, TrackVec *tracks) {
  ScanJob *job = xcalloc(1, sizeof(*job));
  job->kind = kind;
  job->pl_id = p->id;
  job->at = at;
  if (paths) {
    job->paths = *paths;
    memset(paths, 0, sizeof(*paths));
  }
  if (tracks) {
    job->tracks = *tracks;
    memset(tracks, 0, sizeof(*tracks));
  }
  p->scanning++;
  scanner_submit(job);
}

static void apply_job(Playlist *p, ScanJob *job) {
  player_sync_hint(p->id);
  if (job->kind == SCAN_ADD) {
    size_t pos = job->at < 0 ? p->len : (size_t)job->at;
    playlist_insert(p, pos, job->tracks.v, job->tracks.len);
    job->tracks.len = 0;
  } else {
    for (size_t i = 0; i < job->tracks.len; i++) {
      ssize_t idx = playlist_index(p, job->tracks.v[i].id);
      if (idx >= 0) {
        Track old = p->tracks[idx];
        p->tracks[idx] = job->tracks.v[i];
        job->tracks.v[i] = old;
      }
    }
    p->dirty = true;
  }
  player_sync_hint(p->id);
}

void scan_on_done(void *ud) {
  (void)ud;
  ScanJob *job = scanner_take_done();
  while (job) {
    ScanJob *next = job->next;
    Playlist *p = model_find(job->pl_id);
    if (p) {
      apply_job(p, job);
      p->scanning--;
      playlist_changed(p);
      server_notify("scan %u done", p->id);
    }
    scanjob_free(job);
    job = next;
  }
}

void r_track(const Track *t) {
  r_u32("track", t->id);
  r_str("file", track_str(t, TS_PATH));
  if (t->flags & TRACK_CUE) {
    r_u32("begin_ms", t->begin_ms);
    r_opt_u32("end_ms", t->end_ms);
  }
  if (t->flags & TRACK_STREAM) {
    r_str("stream", "1");
  }
  r_opt_u32("dur_ms", t->dur_ms);
  r_opt("artist", track_str(t, TS_ARTIST));
  r_opt("album_artist", track_str(t, TS_ALBUM_ARTIST));
  r_opt("album", track_str(t, TS_ALBUM));
  r_str("title", track_display_title(t));
  r_opt_u32("year", t->year);
  r_opt_u32("number", t->number);
  r_opt_u32("disc", t->disc);
}

static bool playlists(int argc, char **argv) {
  for (size_t i = 0; i < M.len; i++) {
    const Playlist *p = M.pls[i];
    r_u32("playlist", p->id);
    r_str("name", p->name);
    r_u32("count", (uint32_t)p->len);
    r_str("order", order_name(p->order));
    r_opt_u32("scanning", (uint32_t)p->scanning);
  }
  return true;
}

static void pl_new_done(void *arg, bool timed_out) {
  ResolveJob *job = arg;
  if (resolved(job, timed_out)) {
    char *name = job->name ? xstrdup(job->name) : library_playlist_name(job->paths.v, job->paths.len);
    Playlist *p = model_create(name);
    free(name);
    submit(p, SCAN_ADD, -1, &job->paths, NULL);
    r_u32("playlist", p->id);
    list_changed();
  }
  r_finish(job->d, true);
}

static bool pl_new(int argc, char **argv) {
  ResolveJob *job = xcalloc(1, sizeof(*job));
  job->name = strcmp(argv[0], "-") == 0 ? NULL : xstrdup(argv[0]);
  return resolve_async(argc - 1, argv + 1, job, pl_new_done);
}

static void pl_add_done(void *arg, bool timed_out) {
  ResolveJob *job = arg;
  if (resolved(job, timed_out)) {
    Playlist *p = model_find(job->pl);
    if (p) {
      submit(p, SCAN_ADD, job->at, &job->paths, NULL);
    } else {
      r_err(E_NOT_FOUND, "no playlist %u", job->pl);
    }
  }
  r_finish(job->d, true);
}

static bool pl_add(int argc, char **argv) {
  Playlist *p = arg_playlist(argv[0]);
  if (!p) {
    return false;
  }
  argc--;
  argv++;
  int64_t at;
  if (!arg_at(&argc, &argv, &at)) {
    return false;
  }
  if (argc == 0) {
    return r_err(E_SYNTAX, "no paths");
  }
  ResolveJob *job = xcalloc(1, sizeof(*job));
  job->pl = p->id;
  job->at = at;
  return resolve_async(argc, argv, job, pl_add_done);
}

static bool pl_tracks(int argc, char **argv) {
  Playlist *p = arg_playlist(argv[0]);
  if (!p) {
    return false;
  }
  uint32_t from = 0;
  uint32_t count = DEFAULT_PAGE;
  if (argc > 1 && !arg_u32(argv[1], &from, "from")) {
    return false;
  }
  if (argc > 2 && !arg_u32(argv[2], &count, "count")) {
    return false;
  }
  r_u32("total", (uint32_t)p->len);
  for (size_t i = from; i < p->len && i - from < count; i++) {
    r_track(&p->tracks[i]);
  }
  return true;
}

static bool pl_track(int argc, char **argv) {
  Playlist *p = arg_playlist(argv[0]);
  uint32_t id;
  if (!p || !arg_u32(argv[1], &id, "track id")) {
    return false;
  }
  ssize_t idx = playlist_index(p, id);
  if (idx < 0) {
    return r_err(E_NOT_FOUND, "no track %u", id);
  }
  r_u32("pos", (uint32_t)idx);
  r_track(&p->tracks[idx]);
  return true;
}

static bool pl_rm(int argc, char **argv) {
  Playlist *p = arg_playlist(argv[0]);
  uint32_t *ids;
  if (!p || !arg_ids(argc - 1, argv + 1, &ids)) {
    return false;
  }
  player_sync_hint(p->id);
  size_t removed = playlist_remove(p, ids, (size_t)argc - 1);
  if (M.sel_pl == p->id && M.sel_track && playlist_index(p, M.sel_track) < 0) {
    M.sel_track = 0;
    M.dirty = true;
  }
  free(ids);
  r_u32("removed", (uint32_t)removed);
  if (removed) {
    playlist_changed(p);
  }
  return true;
}

static bool pl_move(int argc, char **argv) {
  Playlist *p = arg_playlist(argv[0]);
  uint32_t pos;
  uint32_t *ids;
  if (!p || !arg_u32(argv[1], &pos, "position") || !arg_ids(argc - 2, argv + 2, &ids)) {
    return false;
  }
  size_t moved = playlist_move(p, pos, ids, (size_t)argc - 2);
  free(ids);
  player_sync_hint(p->id);
  r_u32("moved", (uint32_t)moved);
  if (moved) {
    playlist_changed(p);
  }
  return true;
}

static bool pl_copy(int argc, char **argv) {
  Playlist *src = arg_playlist(argv[0]);
  Playlist *dst = src ? arg_playlist(argv[1]) : NULL;
  if (!dst) {
    return false;
  }
  argc -= 2;
  argv += 2;
  int64_t at;
  uint32_t *ids;
  if (!arg_at(&argc, &argv, &at) || !arg_ids(argc, argv, &ids)) {
    return false;
  }
  TrackVec copies = { 0 };
  for (size_t i = 0; i < src->len; i++) {
    for (int j = 0; j < argc; j++) {
      if (src->tracks[i].id == ids[j]) {
        Track t;
        track_copy(&t, &src->tracks[i]);
        t.id = 0;
        trackvec_push(&copies, &t);
        break;
      }
    }
  }
  free(ids);
  player_sync_hint(dst->id);
  playlist_insert(dst, at < 0 ? dst->len : (size_t)at, copies.v, copies.len);
  player_sync_hint(dst->id);
  r_u32("copied", (uint32_t)copies.len);
  free(copies.v);
  if (copies.len) {
    playlist_changed(dst);
  }
  return true;
}

static bool pl_rename(int argc, char **argv) {
  Playlist *p = arg_playlist(argv[0]);
  if (!p) {
    return false;
  }
  if (!*argv[1]) {
    return r_err(E_ARG, "empty name");
  }
  playlist_rename(p, argv[1]);
  list_changed();
  return true;
}

static bool pl_del(int argc, char **argv) {
  Playlist *p = arg_playlist(argv[0]);
  if (!p) {
    return false;
  }
  uint32_t id = p->id;
  ssize_t idx = model_index(id);
  model_delete(id);
  store_playlist_deleted(id);
  player_playlist_deleted(id);
  if (M.sel_pl == id) {
    M.sel_track = 0;
    M.sel_pl = M.len ? M.pls[(size_t)idx < M.len ? (size_t)idx : M.len - 1]->id : 0;
    M.dirty = true;
  }
  list_changed();
  return true;
}

static bool pl_pos(int argc, char **argv) {
  Playlist *p = arg_playlist(argv[0]);
  uint32_t pos;
  if (!p || !arg_u32(argv[1], &pos, "position")) {
    return false;
  }
  model_move(p->id, pos);
  list_changed();
  return true;
}

static bool pl_order(int argc, char **argv) {
  Playlist *p = arg_playlist(argv[0]);
  if (!p) {
    return false;
  }
  if (!order_parse(argv[1], &p->order)) {
    return r_err(E_ARG, "expected inherit|seq|random|noloop");
  }
  p->dirty = true;
  list_changed();
  return true;
}

static bool pl_sort(int argc, char **argv) {
  Playlist *p = arg_playlist(argv[0]);
  if (!p) {
    return false;
  }
  SortKey keys[SORT_MAX_KEYS];
  int nkeys = sort_parse(argc > 1 ? argv[1] : SORT_DEFAULT, keys, SORT_MAX_KEYS);
  if (nkeys <= 0) {
    return r_err(E_ARG, "invalid sort criteria");
  }
  sort_tracks(p->tracks, p->len, keys, nkeys);
  p->dirty = true;
  player_sync_hint(p->id);
  playlist_changed(p);
  return true;
}

static bool pl_reload(int argc, char **argv) {
  Playlist *p = arg_playlist(argv[0]);
  if (!p) {
    return false;
  }
  TrackVec copies = { 0 };
  for (size_t i = 0; i < p->len; i++) {
    if (!(p->tracks[i].flags & (TRACK_STREAM | TRACK_CUE))) {
      Track t;
      track_copy(&t, &p->tracks[i]);
      trackvec_push(&copies, &t);
    }
  }
  submit(p, SCAN_RELOAD, -1, NULL, &copies);
  return true;
}

const Command cmd_playlist[] = {
  { "playlists", playlists, 0, 0 },
  { "pl_new", pl_new, 2, -1 },
  { "pl_add", pl_add, 2, -1 },
  { "pl_tracks", pl_tracks, 1, 3 },
  { "pl_track", pl_track, 2, 2 },
  { "pl_rm", pl_rm, 2, -1 },
  { "pl_move", pl_move, 3, -1 },
  { "pl_copy", pl_copy, 3, -1 },
  { "pl_rename", pl_rename, 2, 2 },
  { "pl_del", pl_del, 1, 1 },
  { "pl_pos", pl_pos, 2, 2 },
  { "pl_order", pl_order, 2, 2 },
  { "pl_sort", pl_sort, 1, 2 },
  { "pl_reload", pl_reload, 1, 1 },
  { NULL, NULL, 0, 0 },
};
