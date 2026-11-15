#ifndef MODEL_MODEL_H
#define MODEL_MODEL_H

#include "model/track.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef enum { ORDER_INHERIT, ORDER_SEQ, ORDER_RANDOM, ORDER_NOLOOP } Order;

typedef struct {
  uint32_t id;
  char *name;
  Order order;
  uint32_t next_track_id;
  Track *tracks;
  size_t len;
  size_t cap;
  int scanning;
  bool dirty;
} Playlist;

typedef struct {
  Playlist **pls;
  size_t len;
  size_t cap;
  uint32_t next_pl_id;
  char *library;
  char *output;
  Order order;
  bool follow;
  int volume;
  uint32_t sel_pl;
  uint32_t sel_track;
  bool dirty;
  bool list_dirty;
} Model;

extern Model M;

const char *order_name(Order o);
bool order_parse(const char *s, Order *out);

Playlist *playlist_new(uint32_t id, const char *name);
void playlist_free(Playlist *p);
ssize_t playlist_index(const Playlist *p, uint32_t track_id);
size_t playlist_insert(Playlist *p, size_t pos, Track *tracks, size_t n);
size_t playlist_remove(Playlist *p, const uint32_t *ids, size_t n);
size_t playlist_move(Playlist *p, size_t pos, const uint32_t *ids, size_t n);
void playlist_rename(Playlist *p, const char *name);

void model_init(void);
void model_free(void);
Playlist *model_find(uint32_t id);
ssize_t model_index(uint32_t id);
Playlist *model_create(const char *name);
void model_add(Playlist *p);
bool model_delete(uint32_t id);
bool model_move(uint32_t id, size_t pos);
Order model_effective_order(const Playlist *p);

#endif
