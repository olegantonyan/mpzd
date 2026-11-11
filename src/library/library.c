#include "library/library.h"
#include "model/model.h"
#include "model/sort.h"
#include "util/util.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static bool is_under(const char *path, const char *root) {
  size_t n = strlen(root);
  if (n == 1 && root[0] == '/') {
    return path[0] == '/';
  }
  return strncmp(path, root, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

const char *library_root_of(const char *resolved) {
  const char *best = NULL;
  for (size_t i = 0; i < M.nroots; i++) {
    if (is_under(resolved, M.roots[i]) && (!best || strlen(M.roots[i]) > strlen(best))) {
      best = M.roots[i];
    }
  }
  return best;
}

char *library_resolve(const char *path) {
  char *real = realpath(path, NULL);
  if (!real) {
    return NULL;
  }
  if (!library_root_of(real)) {
    free(real);
    return NULL;
  }
  return real;
}

int library_root_add(const char *path) {
  char *real = realpath(path, NULL);
  if (!real) {
    return -errno;
  }
  struct stat st;
  if (stat(real, &st) != 0 || !S_ISDIR(st.st_mode)) {
    free(real);
    return -ENOTDIR;
  }
  for (size_t i = 0; i < M.nroots; i++) {
    if (strcmp(M.roots[i], real) == 0) {
      free(real);
      return -EEXIST;
    }
  }
  size_t cap = M.nroots;
  VEC_PUSH(M.roots, M.nroots, cap, real);
  return 0;
}

static LsSort ls_sort_mode;

static int cmp_entries(const void *pa, const void *pb) {
  const LsEntry *a = pa;
  const LsEntry *b = pb;
  if (a->dir != b->dir) {
    return a->dir ? -1 : 1;
  }
  if (ls_sort_mode != LS_NAME && a->mtime != b->mtime) {
    int c = a->mtime < b->mtime ? -1 : 1;
    return ls_sort_mode == LS_DATE_DESC ? -c : c;
  }
  return natcmp(a->name, b->name);
}

int library_ls(const char *dir, LsSort sort, LsEntry **out, size_t *n) {
  DIR *d = opendir(dir);
  if (!d) {
    return -errno;
  }
  LsEntry *v = NULL;
  size_t len = 0, cap = 0;
  StrBuf p = { 0 };
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') {
      continue;
    }
    sb_reset(&p);
    sb_puts(&p, dir);
    sb_puts(&p, "/");
    sb_puts(&p, e->d_name);
    struct stat st;
    if (stat(p.data, &st) != 0) {
      continue;
    }
    bool is_dir = S_ISDIR(st.st_mode);
    if (!is_dir && !(S_ISREG(st.st_mode) && is_listable_file(e->d_name))) {
      continue;
    }
    LsEntry entry = { .name = xstrdup(e->d_name), .dir = is_dir, .mtime = st.st_mtime };
    VEC_PUSH(v, len, cap, entry);
  }
  sb_free(&p);
  closedir(d);
  ls_sort_mode = sort;
  qsort(v, len, sizeof(LsEntry), cmp_entries);
  *out = v;
  *n = len;
  return 0;
}

void library_ls_free(LsEntry *entries, size_t n) {
  for (size_t i = 0; i < n; i++) {
    free(entries[i].name);
  }
  free(entries);
}

char *library_playlist_name(char *const *paths, size_t n) {
  StrBuf name = { 0 };
  for (size_t i = 0; i < n; i++) {
    const char *p = paths[i];
    if (!path_is_url(p)) {
      const char *root = library_root_of(p);
      if (root && strcmp(root, p) != 0) {
        p += strlen(root);
        while (*p == '/') p++;
      } else {
        p = path_basename(p);
      }
    }
    if (i) {
      sb_puts(&name, ", ");
    }
    sb_puts(&name, p);
  }
  if (!name.data) {
    sb_puts(&name, "New playlist");
  }
  return name.data;
}
