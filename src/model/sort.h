#ifndef MODEL_SORT_H
#define MODEL_SORT_H

#include "model/track.h"

#include <stdbool.h>
#include <stddef.h>

#define SORT_DEFAULT "YEAR/ALBUM/DIRECTORY/DISCNUMBER/TRACKNUMBER/FILENAME/TITLE"
#define SORT_MAX_KEYS 16

typedef enum {
  SK_ARTIST, SK_ALBUMARTIST, SK_ALBUM, SK_YEAR, SK_TRACKNUMBER, SK_DISCNUMBER, SK_FILENAME, SK_TITLE, SK_DIRECTORY
} SortField;

typedef struct {
  SortField field;
  bool desc;
} SortKey;

int natcmp_n(const char *a, size_t alen, const char *b, size_t blen);
int natcmp(const char *a, const char *b);
int sort_parse(const char *criteria, SortKey *keys, int max);
void sort_tracks(Track *tracks, size_t n, const SortKey *keys, int nkeys);
void sort_tracks_default(Track *tracks, size_t n);

#endif
