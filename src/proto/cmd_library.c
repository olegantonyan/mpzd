#include "library/library.h"
#include "model/model.h"
#include "proto/proto.h"
#include "util/util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static bool library(int argc, char **argv) {
  r_str("library", M.library);
  return true;
}

typedef struct {
  Deferred *d;
  char *root;
  char *path;
  LsSort sort;
  bool found;
  int rc;
  LsEntry *entries;
  size_t n;
} LsJob;

static void ls_work(void *arg) {
  LsJob *job = arg;
  char *dir = library_resolve(job->root, job->path);
  job->found = dir != NULL;
  if (dir) {
    job->rc = library_ls(dir, job->sort, &job->entries, &job->n);
    free(dir);
  }
}

static void ls_reply(const LsJob *job) {
  if (!job->found) {
    r_err(E_NOT_FOUND, "not found or outside the library");
  } else if (job->rc < 0) {
    r_err(job->rc == -ENOTDIR ? E_ARG : E_IO, "%s", strerror(-job->rc));
  } else {
    for (size_t i = 0; i < job->n; i++) {
      r_str(job->entries[i].dir ? "dir" : "file", job->entries[i].name);
    }
  }
}

static void ls_done(void *arg, bool timed_out) {
  LsJob *job = arg;
  if (r_resume(job->d, timed_out)) {
    ls_reply(job);
  }
  r_finish(job->d, true);
}

static void ls_free(void *arg) {
  LsJob *job = arg;
  if (job->rc == 0) {
    library_ls_free(job->entries, job->n);
  }
  free(job->root);
  free(job->path);
  free(job);
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
  LsJob *job = xcalloc(1, sizeof(*job));
  job->root = xstrdup(M.library);
  job->path = xstrdup(path);
  job->sort = sort;
  return r_async(job, &job->d, ls_work, ls_done, ls_free, FS_TIMEOUT_MS);
}

const Command cmd_library[] = {
  { "library", library, 0, 0 },
  { "ls", ls, 0, 2 },
  { NULL, NULL, 0, 0 },
};
