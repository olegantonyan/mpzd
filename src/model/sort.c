#include "model/sort.h"
#include "util/util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int natcmp_n(const char *a, size_t alen, const char *b, size_t blen) {
  size_t i = 0;
  size_t j = 0;
  while (i < alen && j < blen) {
    unsigned char ca = (unsigned char)a[i];
    unsigned char cb = (unsigned char)b[j];
    if (isdigit(ca) && isdigit(cb)) {
      while (i < alen && a[i] == '0') i++;
      while (j < blen && b[j] == '0') j++;
      size_t si = i;
      size_t sj = j;
      while (i < alen && isdigit((unsigned char)a[i])) i++;
      while (j < blen && isdigit((unsigned char)b[j])) j++;
      size_t la = i - si;
      size_t lb = j - sj;
      if (la != lb) {
        return la < lb ? -1 : 1;
      }
      int c = memcmp(a + si, b + sj, la);
      if (c) {
        return c < 0 ? -1 : 1;
      }
      continue;
    }
    int la = tolower(ca);
    int lb = tolower(cb);
    if (la != lb) {
      return la < lb ? -1 : 1;
    }
    i++;
    j++;
  }
  if (i < alen) {
    return 1;
  }
  if (j < blen) {
    return -1;
  }
  return 0;
}

int natcmp(const char *a, const char *b) {
  return natcmp_n(a, strlen(a), b, strlen(b));
}

static const char *const field_names[] = {
  "ARTIST", "ALBUMARTIST", "ALBUM", "YEAR", "TRACKNUMBER", "DISCNUMBER", "FILENAME", "TITLE", "DIRECTORY"
};

int sort_parse(const char *criteria, SortKey *keys, int max) {
  int n = 0;
  const char *p = criteria;
  while (*p) {
    const char *end = strchr(p, '/');
    if (!end) {
      end = p + strlen(p);
    }
    const char *s = p;
    const char *e = end;
    while (s < e && isspace((unsigned char)*s)) s++;
    while (e > s && isspace((unsigned char)e[-1])) e--;
    bool desc = false;
    if (s < e && *s == '-') {
      desc = true;
      s++;
    }
    if (s == e) {
      return -1;
    }
    size_t len = (size_t)(e - s);
    int field = -1;
    for (size_t i = 0; i < ARRAY_LEN(field_names); i++) {
      if (strlen(field_names[i]) == len && strncasecmp(field_names[i], s, len) == 0) {
        field = (int)i;
        break;
      }
    }
    if (field < 0 || n >= max) {
      return -1;
    }
    keys[n].field = (SortField)field;
    keys[n].desc = desc;
    n++;
    p = *end ? end + 1 : end;
  }
  return n;
}

static int cmp_num(unsigned a, unsigned b) {
  return a < b ? -1 : (a > b ? 1 : 0);
}

static int cmp_field(const Track *a, const Track *b, SortField f) {
  const char *pa = track_str(a, TS_PATH);
  const char *pb = track_str(b, TS_PATH);
  switch (f) {
    case SK_ARTIST: return strcoll(track_str(a, TS_ARTIST), track_str(b, TS_ARTIST));
    case SK_ALBUMARTIST: return strcoll(track_str(a, TS_ALBUM_ARTIST), track_str(b, TS_ALBUM_ARTIST));
    case SK_ALBUM: return strcoll(track_str(a, TS_ALBUM), track_str(b, TS_ALBUM));
    case SK_TITLE: return strcoll(track_str(a, TS_TITLE), track_str(b, TS_TITLE));
    case SK_YEAR: return cmp_num(a->year, b->year);
    case SK_TRACKNUMBER: return cmp_num(a->number, b->number);
    case SK_DISCNUMBER: return cmp_num(a->disc, b->disc);
    case SK_FILENAME: return natcmp(path_basename(pa), path_basename(pb));
    case SK_DIRECTORY: return natcmp_n(pa, path_dirname_len(pa), pb, path_dirname_len(pb));
  }
  return 0;
}

static int cmp_tracks(const Track *a, const Track *b, const SortKey *keys, int nkeys) {
  for (int i = 0; i < nkeys; i++) {
    int c = cmp_field(a, b, keys[i].field);
    if (c) {
      return keys[i].desc ? -c : c;
    }
  }
  if (a->begin_ms != b->begin_ms && strcmp(track_str(a, TS_PATH), track_str(b, TS_PATH)) == 0) {
    return a->begin_ms < b->begin_ms ? -1 : 1;
  }
  return 0;
}

static void merge_sort(Track *v, Track *tmp, size_t n, const SortKey *keys, int nkeys) {
  if (n < 2) {
    return;
  }
  size_t mid = n / 2;
  merge_sort(v, tmp, mid, keys, nkeys);
  merge_sort(v + mid, tmp, n - mid, keys, nkeys);
  if (cmp_tracks(&v[mid - 1], &v[mid], keys, nkeys) <= 0) {
    return;
  }
  memcpy(tmp, v, mid * sizeof(Track));
  size_t i = 0;
  size_t j = mid;
  size_t k = 0;
  while (i < mid && j < n) {
    if (cmp_tracks(&v[j], &tmp[i], keys, nkeys) < 0) {
      v[k++] = v[j++];
    } else {
      v[k++] = tmp[i++];
    }
  }
  while (i < mid) {
    v[k++] = tmp[i++];
  }
}

void sort_tracks(Track *tracks, size_t n, const SortKey *keys, int nkeys) {
  if (n < 2) {
    return;
  }
  Track *tmp = xmalloc((n / 2 + 1) * sizeof(Track));
  merge_sort(tracks, tmp, n, keys, nkeys);
  free(tmp);
}

void sort_tracks_default(Track *tracks, size_t n) {
  SortKey keys[SORT_MAX_KEYS];
  int nkeys = sort_parse(SORT_DEFAULT, keys, SORT_MAX_KEYS);
  sort_tracks(tracks, n, keys, nkeys);
}
