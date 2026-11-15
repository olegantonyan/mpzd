#ifndef STORE_STORE_H
#define STORE_STORE_H

#include "model/model.h"
#include "util/util.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
  bool valid;
  uint32_t pl;
  uint32_t track;
  uint32_t pos_ms;
} StorePlaying;

bool store_init(const char *dir);
void store_free(void);
bool store_load(StorePlaying *playing);
bool store_flush(const StorePlaying *playing, bool force_state);
void store_playlist_deleted(uint32_t id);

bool store_write_playlist(FILE *f, const Playlist *p);
Playlist *store_read_playlist(FILE *f);

#endif
