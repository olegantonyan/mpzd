#include "library/library.h"
#include "model/model.h"
#include "proto/proto.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static bool library(int argc, char **argv) {
  r_str("library", M.library);
  return true;
}

static bool ls(int argc, char **argv) {
  const char *path = M.library;
  if (argc > 0 && argv[0][0] == '/') {
    path = argv[0];
    argc--;
    argv++;
  }
  if (argc > 1) {
    return r_err(E_ARG, "expected [path] [name|date|-date]");
  }
  LsSort sort = LS_NAME;
  if (argc > 0) {
    if (strcmp(argv[0], "name") == 0) {
      sort = LS_NAME;
    } else if (strcmp(argv[0], "date") == 0) {
      sort = LS_DATE;
    } else if (strcmp(argv[0], "-date") == 0) {
      sort = LS_DATE_DESC;
    } else {
      return r_err(E_ARG, "expected name|date|-date");
    }
  }
  char *dir = library_resolve(path);
  if (!dir) {
    return r_err(E_NOT_FOUND, "not found or outside the library");
  }
  LsEntry *entries;
  size_t n;
  int rc = library_ls(dir, sort, &entries, &n);
  free(dir);
  if (rc < 0) {
    return r_err(rc == -ENOTDIR ? E_ARG : E_IO, "%s", strerror(-rc));
  }
  for (size_t i = 0; i < n; i++) {
    r_str(entries[i].dir ? "dir" : "file", entries[i].name);
  }
  library_ls_free(entries, n);
  return true;
}

const Command cmd_library[] = {
  { "library", library, 0, 0 },
  { "ls", ls, 0, 2 },
  { NULL, NULL, 0, 0 },
};
