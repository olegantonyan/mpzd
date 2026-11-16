#include "library/cue.h"
#include "library/m3u.h"
#include "model/model.h"
#include "model/sort.h"
#include "proto/proto.h"
#include "store/store.h"
#include "util/bg.h"
#include "util/util.h"

#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
static int checks;

#define CHECK(cond) do { \
  checks++; \
  if (!(cond)) { \
    failures++; \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
  } \
} while (0)

#define CHECK_STR(a, b) do { \
  checks++; \
  const char *ca__ = (a); \
  const char *cb__ = (b); \
  if (!ca__ || !cb__ || strcmp(ca__, cb__) != 0) { \
    failures++; \
    fprintf(stderr, "%s:%d: '%s' != '%s'\n", __FILE__, __LINE__, ca__ ? ca__ : "(null)", cb__ ? cb__ : "(null)"); \
  } \
} while (0)

static char tmpdir[256];

static void write_file(const char *name, const char *content) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", tmpdir, name);
  FILE *f = fopen(path, "w");
  fputs(content, f);
  fclose(f);
}

static Track mk(const char *path, const char *album, uint16_t year, uint16_t number) {
  TrackInfo info = { .str = { [TS_PATH] = path, [TS_ALBUM] = album }, .year = year, .number = number };
  Track t;
  track_init(&t, &info);
  return t;
}

static void test_tokenize(void) {
  char line[] = "pl_new  - \"/music/a b/c\" x \"q\\\"uote\" \"\"";
  char *argv[8];
  int argc = proto_tokenize(line, argv, 8);
  CHECK(argc == 6);
  CHECK_STR(argv[0], "pl_new");
  CHECK_STR(argv[1], "-");
  CHECK_STR(argv[2], "/music/a b/c");
  CHECK_STR(argv[3], "x");
  CHECK_STR(argv[4], "q\"uote");
  CHECK_STR(argv[5], "");

  char unterminated[] = "ls \"/music";
  CHECK(proto_tokenize(unterminated, argv, 8) == -1);
  char glued[] = "ls \"/a\"b";
  CHECK(proto_tokenize(glued, argv, 8) == -1);
  char too_many[] = "a b c";
  CHECK(proto_tokenize(too_many, argv, 2) == -1);
  char empty[] = "   ";
  CHECK(proto_tokenize(empty, argv, 8) == 0);
  char escaped_trailing[] = "x \"a\\\\\"";
  CHECK(proto_tokenize(escaped_trailing, argv, 8) == 2);
  CHECK_STR(argv[1], "a\\");
}

static void test_natcmp(void) {
  CHECK(natcmp("track2", "track10") < 0);
  CHECK(natcmp("Track10", "track2") > 0);
  CHECK(natcmp("a", "A") == 0);
  CHECK(natcmp("01 x", "1 x") == 0);
  CHECK(natcmp("abc", "abcd") < 0);
  CHECK(natcmp_n("/m/a/1.mp3", 4, "/m/b/1.mp3", 4) < 0);
}

static void test_sort(void) {
  SortKey keys[SORT_MAX_KEYS];
  CHECK(sort_parse(SORT_DEFAULT, keys, SORT_MAX_KEYS) == 7);
  int n = sort_parse(" -YEAR / album", keys, SORT_MAX_KEYS);
  CHECK(n == 2);
  CHECK(keys[0].field == SK_YEAR && keys[0].desc);
  CHECK(keys[1].field == SK_ALBUM && !keys[1].desc);
  CHECK(sort_parse("YEAR/FOO", keys, SORT_MAX_KEYS) == -1);
  CHECK(sort_parse("YEAR//ALBUM", keys, SORT_MAX_KEYS) == -1);

  Track t[5] = {
    mk("/m/b/10.flac", "B", 2001, 10),
    mk("/m/b/2.flac", "B", 2001, 2),
    mk("/m/a/1.flac", "A", 1999, 1),
    mk("/m/c/x.flac", "C", 2001, 0),
    mk("/m/c/y.flac", "C", 2001, 0),
  };
  sort_tracks_default(t, 5);
  CHECK_STR(track_str(&t[0], TS_PATH), "/m/a/1.flac");
  CHECK_STR(track_str(&t[1], TS_PATH), "/m/b/2.flac");
  CHECK_STR(track_str(&t[2], TS_PATH), "/m/b/10.flac");
  CHECK_STR(track_str(&t[3], TS_PATH), "/m/c/x.flac");
  CHECK_STR(track_str(&t[4], TS_PATH), "/m/c/y.flac");

  n = sort_parse("-YEAR", keys, SORT_MAX_KEYS);
  sort_tracks(t, 5, keys, n);
  CHECK_STR(track_str(&t[0], TS_PATH), "/m/b/2.flac");
  CHECK_STR(track_str(&t[3], TS_PATH), "/m/c/y.flac");
  CHECK_STR(track_str(&t[4], TS_PATH), "/m/a/1.flac");
  for (int i = 0; i < 5; i++) {
    track_free(&t[i]);
  }
}

static void test_playlist_ops(void) {
  Playlist *p = playlist_new(1, "p");
  Track t[4] = { mk("/a", "", 0, 0), mk("/b", "", 0, 0), mk("/c", "", 0, 0), mk("/d", "", 0, 0) };
  playlist_insert(p, 0, t, 4);
  CHECK(p->len == 4 && p->tracks[0].id == 1 && p->tracks[3].id == 4);
  CHECK(p->next_track_id == 5);

  uint32_t mv[] = { 4, 1 };
  CHECK(playlist_move(p, 1, mv, 2) == 2);
  CHECK(p->tracks[0].id == 2 && p->tracks[1].id == 1 && p->tracks[2].id == 4 && p->tracks[3].id == 3);

  uint32_t rm[] = { 1, 3, 99 };
  CHECK(playlist_remove(p, rm, 3) == 2);
  CHECK(p->len == 2 && p->tracks[0].id == 2 && p->tracks[1].id == 4);
  CHECK(playlist_index(p, 4) == 1 && playlist_index(p, 1) == -1);

  Track more = mk("/e", "", 0, 0);
  playlist_insert(p, 99, &more, 1);
  CHECK(p->len == 3 && p->tracks[2].id == 5);
  playlist_free(p);
}

static uint32_t fake_duration(const char *path) {
  return 600000;
}

static void test_cue(void) {
  write_file("album.flac", "");
  write_file("Disc2.FLAC", "");
  write_file("album.cue",
    "\xEF\xBB\xBFREM DATE 1994\r\n"
    "REM DISCNUMBER 1\r\n"
    "PERFORMER \"Band\"\r\n"
    "TITLE \"Album Name\"\r\n"
    "FILE \"album.wav\" WAVE\r\n"
    "  TRACK 01 AUDIO\r\n"
    "    TITLE \"One\"\r\n"
    "    INDEX 01 00:00:00\r\n"
    "  TRACK 02 AUDIO\r\n"
    "    TITLE \"Two\"\r\n"
    "    PERFORMER \"Guest\"\r\n"
    "    INDEX 00 03:59:00\r\n"
    "    INDEX 01 04:00:37\r\n"
    "FILE \"disc2.flac\" WAVE\r\n"
    "  TRACK 03 AUDIO\r\n"
    "    TITLE \"Three\"\r\n"
    "    INDEX 00 00:01:00\r\n");

  char cue[512];
  snprintf(cue, sizeof(cue), "%s/album.cue", tmpdir);
  TrackVec tv = { 0 };
  StrVec files = { 0 };
  CHECK(cue_parse(cue, fake_duration, &tv, &files));
  CHECK(tv.len == 3);
  CHECK(files.len == 2);
  if (tv.len == 3 && files.len == 2) {
    CHECK(strstr(files.v[0], "/album.flac") != NULL);
    CHECK(strstr(files.v[1], "/Disc2.FLAC") != NULL);
    CHECK_STR(track_str(&tv.v[0], TS_TITLE), "One");
    CHECK_STR(track_str(&tv.v[0], TS_ARTIST), "Band");
    CHECK_STR(track_str(&tv.v[0], TS_ALBUM), "Album Name");
    CHECK(tv.v[0].begin_ms == 0 && tv.v[0].end_ms == 240493);
    CHECK(tv.v[0].dur_ms == 240493);
    CHECK(tv.v[0].year == 1994 && tv.v[0].disc == 1 && tv.v[0].number == 1);
    CHECK((tv.v[0].flags & TRACK_CUE) != 0);
    CHECK_STR(track_str(&tv.v[1], TS_ARTIST), "Guest");
    CHECK_STR(track_str(&tv.v[1], TS_ALBUM_ARTIST), "Band");
    CHECK(tv.v[1].begin_ms == 240493 && tv.v[1].end_ms == 0);
    CHECK(tv.v[1].dur_ms == 600000 - 240493);
    CHECK(tv.v[2].begin_ms == 1000 && tv.v[2].end_ms == 0 && tv.v[2].dur_ms == 599000);
    CHECK_STR(track_str(&tv.v[2], TS_PATH), files.v[1]);
  }
  trackvec_free(&tv);
  strvec_free(&files);
}

static void test_m3u(void) {
  write_file("list.m3u8", "#EXTM3U\n#EXTINF:1,x\nsub/a.mp3\n/abs/b.flac\nhttp://radio/stream\n\n");
  write_file("radio.pls", "[playlist]\nFile1=http://one\nTitle1=One\nFile2=two.ogg\nNumberOfEntries=2\n");
  char path[512];
  StrVec e = { 0 };
  snprintf(path, sizeof(path), "%s/list.m3u8", tmpdir);
  CHECK(m3u_parse(path, &e));
  CHECK(e.len == 3);
  if (e.len == 3) {
    char want[512];
    snprintf(want, sizeof(want), "%s/sub/a.mp3", tmpdir);
    CHECK_STR(e.v[0], want);
    CHECK_STR(e.v[1], "/abs/b.flac");
    CHECK_STR(e.v[2], "http://radio/stream");
  }
  strvec_free(&e);
  snprintf(path, sizeof(path), "%s/radio.pls", tmpdir);
  CHECK(m3u_parse(path, &e));
  CHECK(e.len == 2);
  if (e.len == 2) {
    CHECK_STR(e.v[0], "http://one");
    CHECK(strstr(e.v[1], "/two.ogg") != NULL);
  }
  strvec_free(&e);
}

static void test_store_roundtrip(void) {
  Playlist *p = playlist_new(7, "tab\there\\ and\nnewline");
  p->order = ORDER_NOLOOP;
  TrackInfo info = {
    .str = { [TS_PATH] = "/m/x\ty.flac", [TS_ARTIST] = "A\\B", [TS_TITLE] = "line1\nline2", [TS_ALBUM] = "" },
    .begin_ms = 1000, .end_ms = 2000, .dur_ms = 1000, .year = 2020, .number = 3, .disc = 2, .flags = TRACK_CUE,
  };
  Track t;
  track_init(&t, &info);
  playlist_insert(p, 0, &t, 1);

  FILE *f = tmpfile();
  CHECK(store_write_playlist(f, p));
  rewind(f);
  Playlist *q = store_read_playlist(f);
  fclose(f);
  CHECK(q != NULL);
  if (q) {
    CHECK(q->id == 7 && q->order == ORDER_NOLOOP && q->next_track_id == 2);
    CHECK_STR(q->name, p->name);
    CHECK(q->len == 1);
    if (q->len == 1) {
      const Track *r = &q->tracks[0];
      CHECK(r->id == 1 && r->begin_ms == 1000 && r->end_ms == 2000 && r->dur_ms == 1000);
      CHECK(r->year == 2020 && r->number == 3 && r->disc == 2 && r->flags == TRACK_CUE);
      CHECK_STR(track_str(r, TS_PATH), "/m/x\ty.flac");
      CHECK_STR(track_str(r, TS_ARTIST), "A\\B");
      CHECK_STR(track_str(r, TS_TITLE), "line1\nline2");
    }
  }
  playlist_free(p);
  playlist_free(q);
}

static Playlist *store_roundtrip(const Playlist *p, long cut, long flip) {
  FILE *f = tmpfile();
  CHECK(store_write_playlist(f, p) && fflush(f) == 0);
  long size = ftell(f);
  if (cut > 0) {
    CHECK(ftruncate(fileno(f), size - cut) == 0);
  }
  if (flip > 0) {
    fseek(f, size - flip, SEEK_SET);
    int c = fgetc(f);
    fseek(f, size - flip, SEEK_SET);
    fputc(c ^ 0x5a, f);
  }
  rewind(f);
  Playlist *q = store_read_playlist(f);
  fclose(f);
  return q;
}

static void test_store_files(void) {
  Playlist *p = playlist_new(3, "many");
  const char *paths[] = { "/m/Artist/Album/01.flac", "/m/Artist/Album/02.flac", "/m/Other/X/01.flac", "/m/Other/X/01.flac" };
  const char *artists[] = { "Artist", "Artist", "Other", "" };
  for (int i = 0; i < 4; i++) {
    TrackInfo info = {
      .str = { [TS_PATH] = paths[i], [TS_ARTIST] = artists[i], [TS_ALBUM_ARTIST] = artists[i], [TS_ALBUM] = i < 2 ? "Album" : "X", [TS_TITLE] = i == 3 ? "" : "t" },
      .dur_ms = 1000u * (unsigned)i, .year = 1999, .number = (uint16_t)(i + 1),
    };
    Track t;
    track_init(&t, &info);
    playlist_insert(p, (size_t)i, &t, 1);
  }
  uint32_t last = p->tracks[3].id;
  playlist_move(p, 0, &last, 1);

  Playlist *q = store_roundtrip(p, 0, 0);
  CHECK(q != NULL);
  if (q) {
    CHECK(q->len == 4 && q->next_track_id == p->next_track_id);
    for (size_t i = 0; i < q->len && i < p->len; i++) {
      CHECK(q->tracks[i].id == p->tracks[i].id && q->tracks[i].dur_ms == p->tracks[i].dur_ms && q->tracks[i].number == p->tracks[i].number);
      for (int k = 0; k < TS_COUNT; k++) {
        CHECK_STR(track_str(&q->tracks[i], k), track_str(&p->tracks[i], k));
      }
    }
  }
  playlist_free(q);

  CHECK(store_roundtrip(p, 1, 0) == NULL);
  CHECK(store_roundtrip(p, 0, 3) == NULL);

  FILE *plain = tmpfile();
  fputs("mpzd-playlist 1\n5\tplain\tseq\t2\n1\t/m/a.flac\t0\t0\t10\tA\tA\tB\tT\t2001\t1\t0\t0\n", plain);
  rewind(plain);
  q = store_read_playlist(plain);
  fclose(plain);
  CHECK(q != NULL && q->id == 5 && q->len == 1 && q->order == ORDER_SEQ);
  if (q && q->len == 1) {
    CHECK_STR(track_str(&q->tracks[0], TS_PATH), "/m/a.flac");
    CHECK(q->tracks[0].year == 2001 && q->tracks[0].dur_ms == 10);
  }
  playlist_free(q);

  Playlist *e = playlist_new(9, "");
  q = store_roundtrip(e, 0, 0);
  CHECK(q != NULL && q->id == 9 && q->len == 0 && strcmp(q->name, "") == 0);
  playlist_free(q);
  playlist_free(e);
  playlist_free(p);
}

static int bg_done_calls;
static int bg_timeouts;
static atomic_int bg_frees;

typedef struct {
  unsigned sleep_ms;
  int result;
} BgTest;

static void bgt_work(void *arg) {
  BgTest *t = arg;
  usleep(t->sleep_ms * 1000);
  t->result = 42;
}

static void bgt_done(void *arg, bool timed_out) {
  bg_done_calls++;
  if (timed_out) {
    bg_timeouts++;
  } else {
    CHECK(((BgTest *)arg)->result == 42);
  }
}

static void bgt_free(void *arg) {
  atomic_fetch_add(&bg_frees, 1);
  free(arg);
}

static BgTest *bgt(unsigned sleep_ms) {
  BgTest *t = xcalloc(1, sizeof(*t));
  t->sleep_ms = sleep_ms;
  return t;
}

static void bg_pump(int efd, int tfd, int want_done, unsigned max_ms) {
  uint64_t until = mono_ms() + max_ms;
  while (bg_done_calls < want_done && mono_ms() < until) {
    struct pollfd fds[2] = { { .fd = efd, .events = POLLIN }, { .fd = tfd, .events = POLLIN } };
    if (poll(fds, 2, 50) > 0) {
      if (fds[0].revents & POLLIN) {
        bg_on_event(NULL);
      }
      if (fds[1].revents & POLLIN) {
        bg_on_timer(NULL);
      }
    }
  }
}

static void test_bg(void) {
  int efd, tfd;
  CHECK(bg_init(&efd, &tfd));
  CHECK(bg_run(bgt(10), bgt_work, bgt_done, bgt_free, 1000));
  bg_pump(efd, tfd, 1, 2000);
  CHECK(bg_done_calls == 1 && bg_timeouts == 0 && atomic_load(&bg_frees) == 1);

  CHECK(bg_run(bgt(400), bgt_work, bgt_done, bgt_free, 50));
  bg_pump(efd, tfd, 2, 2000);
  CHECK(bg_done_calls == 2 && bg_timeouts == 1 && atomic_load(&bg_frees) == 1);
  usleep(600 * 1000);
  CHECK(atomic_load(&bg_frees) == 2);

  int started = 0;
  for (int i = 0; i < 32; i++) {
    BgTest *t = bgt(300);
    if (bg_run(t, bgt_work, bgt_done, bgt_free, 20)) {
      started++;
    } else {
      free(t);
    }
  }
  CHECK(started == 16);
  bg_pump(efd, tfd, 2 + started, 2000);
  CHECK(bg_done_calls == 2 + started && bg_timeouts == 1 + started);
  usleep(500 * 1000);
  CHECK(atomic_load(&bg_frees) == 2 + started);
  CHECK(bg_run(bgt(0), bgt_work, bgt_done, bgt_free, 1000));
  bg_pump(efd, tfd, 3 + started, 2000);
  CHECK(bg_done_calls == 3 + started);
  bg_shutdown();
}

static Playlist *read_back(const char *dir, uint32_t id) {
  char path[600];
  snprintf(path, sizeof(path), "%s/playlists/%u.tsv.gz", dir, id);
  FILE *f = fopen(path, "r");
  if (!f) {
    return NULL;
  }
  Playlist *p = store_read_playlist(f);
  fclose(f);
  return p;
}

static void test_store_writer(void) {
  char dir[400];
  snprintf(dir, sizeof(dir), "%s/state", tmpdir);
  model_init();
  CHECK(store_init(dir));
  Playlist *p = model_create("first");
  Track t = mk("/m/a.flac", "A", 2000, 1);
  playlist_insert(p, 0, &t, 1);
  p->dirty = true;
  CHECK(store_flush(NULL, true));
  free(p->name);
  p->name = xstrdup("second");
  p->dirty = true;
  CHECK(store_flush(NULL, false));
  CHECK(store_shutdown(5000));
  Playlist *q = read_back(dir, p->id);
  CHECK(q != NULL && strcmp(q->name, "second") == 0 && q->len == 1);
  playlist_free(q);
  char state[600];
  snprintf(state, sizeof(state), "%s/state", dir);
  CHECK(access(state, F_OK) == 0);

  store_free();
  CHECK(store_init(dir));
  store_playlist_deleted(p->id);
  CHECK(store_shutdown(5000));
  CHECK(read_back(dir, p->id) == NULL);

  if (geteuid() != 0) {
    store_free();
    CHECK(store_init(dir));
    char pl_dir[600];
    snprintf(pl_dir, sizeof(pl_dir), "%s/playlists", dir);
    chmod(pl_dir, 0500);
    p->dirty = true;
    uint64_t t0 = mono_ms();
    CHECK(store_flush(NULL, false));
    CHECK(mono_ms() - t0 < 1000);
    CHECK(!store_shutdown(300));
    chmod(pl_dir, 0700);
  }
  store_free();
  model_free();
}

int main(void) {
  snprintf(tmpdir, sizeof(tmpdir), "/tmp/mpzd-test-XXXXXX");
  const char *base = getenv("TMPDIR");
  if (base && *base) {
    snprintf(tmpdir, sizeof(tmpdir), "%s/mpzd-test-XXXXXX", base);
  }
  if (!mkdtemp(tmpdir)) {
    perror("mkdtemp");
    return 1;
  }

  test_tokenize();
  test_natcmp();
  test_sort();
  test_playlist_ops();
  test_cue();
  test_m3u();
  test_store_roundtrip();
  test_store_files();
  test_bg();
  test_store_writer();

  char cmd[600];
  snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
  if (system(cmd) != 0) {
    fprintf(stderr, "cannot remove %s\n", tmpdir);
  }

  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
