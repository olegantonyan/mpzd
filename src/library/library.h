#ifndef LIBRARY_LIBRARY_H
#define LIBRARY_LIBRARY_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

typedef enum { LS_NAME, LS_DATE, LS_DATE_DESC } LsSort;

typedef struct {
  char *name;
  bool dir;
  time_t mtime;
} LsEntry;

char *library_resolve(const char *path);
int library_root_add(const char *path);
const char *library_root_of(const char *resolved);
int library_ls(const char *dir, LsSort sort, LsEntry **out, size_t *n);
void library_ls_free(LsEntry *entries, size_t n);
char *library_playlist_name(char *const *paths, size_t n);

#endif
