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

#define LOG_MODULE "library"

static bool is_under(const char *path, const char *root) {
  size_t n = strlen(root);
  if (n == 1 && root[0] == '/') {
    return path[0] == '/';
  }
  return strncmp(path, root, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

char *library_resolve(const char *root, const char *path) {
  char *real = realpath(path, NULL);
  if (!real) {
    return NULL;
  }
  if (!root || !is_under(real, root)) {
    free(real);
    return NULL;
  }
  return real;
}

int library_set(const char *path) {
  char *real = realpath(path, NULL);
  if (!real) {
    int err = errno;
    if (path[0] != '/') {
      return -err;
    }
    LOG("library %s unavailable: %s", path, strerror(err));
    real = xstrdup(path);
    for (size_t n = strlen(real); n > 1 && real[n - 1] == '/'; n--) {
      real[n - 1] = '\0';
    }
  } else {
    struct stat st;
    if (stat(real, &st) != 0 || !S_ISDIR(st.st_mode)) {
      free(real);
      return -ENOTDIR;
    }
  }
  free(M.library);
  M.library = real;
  return 0;
}

static int cmp_entries(const void *pa, const void *pb, void *arg) {
  const LsEntry *a = pa;
  const LsEntry *b = pb;
  LsSort ls_sort_mode = *(const LsSort *)arg;
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
  qsort_r(v, len, sizeof(LsEntry), cmp_entries, &sort);
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
      if (M.library && is_under(p, M.library) && strcmp(M.library, p) != 0) {
        p += strlen(M.library);
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
