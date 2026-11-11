#include "library/library.h"
#include "model/model.h"
#include "proto/proto.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static bool libraries(int argc, char **argv) {
  for (size_t i = 0; i < M.nroots; i++) {
    r_str("library", M.roots[i]);
  }
  return true;
}

static bool ls(int argc, char **argv) {
  LsSort sort = LS_NAME;
  if (argc > 1) {
    if (strcmp(argv[1], "name") == 0) {
      sort = LS_NAME;
    } else if (strcmp(argv[1], "date") == 0) {
      sort = LS_DATE;
    } else if (strcmp(argv[1], "-date") == 0) {
      sort = LS_DATE_DESC;
    } else {
      return r_err(E_ARG, "expected name|date|-date");
    }
  }
  char *dir = library_resolve(argv[0]);
  if (!dir) {
    return r_err(E_NOT_FOUND, "not found or outside libraries");
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
  { "libraries", libraries, 0, 0 },
  { "ls", ls, 1, 2 },
  { NULL, NULL, 0, 0 },
};
