#include "library/cue.h"
#include "library/m3u.h"
#include "model/model.h"
#include "model/sort.h"
#include "proto/proto.h"
#include "store/store.h"
#include "util/util.h"

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

  char cmd[600];
  snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
  if (system(cmd) != 0) {
    fprintf(stderr, "cannot remove %s\n", tmpdir);
  }

  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
