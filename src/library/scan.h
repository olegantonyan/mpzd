#ifndef LIBRARY_SCAN_H
#define LIBRARY_SCAN_H

#include "model/track.h"
#include "util/util.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum { SCAN_ADD, SCAN_RELOAD } ScanKind;

typedef struct ScanJob {
  ScanKind kind;
  uint32_t pl_id;
  int64_t at;
  StrVec paths;
  TrackVec tracks;
  struct ScanJob *next;
} ScanJob;

void scan_path(const char *path, TrackVec *out);
void scan_reload(TrackVec *tracks);

bool scanner_start(int *event_fd);
void scanner_stop(void);
void scanner_submit(ScanJob *job);
ScanJob *scanner_take_done(void);
void scanner_watchdog(void);
void scanjob_free(ScanJob *job);

#endif
