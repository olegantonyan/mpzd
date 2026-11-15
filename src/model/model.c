#include "model/model.h"
#include "util/util.h"

#include <stdlib.h>
#include <string.h>

Model M;

static const char *const order_names[] = { "inherit", "seq", "random", "noloop" };

const char *order_name(Order o) {
  return order_names[o];
}

bool order_parse(const char *s, Order *out) {
  for (size_t i = 0; i < ARRAY_LEN(order_names); i++) {
    if (strcmp(s, order_names[i]) == 0) {
      *out = (Order)i;
      return true;
    }
  }
  return false;
}

Playlist *playlist_new(uint32_t id, const char *name) {
  Playlist *p = xcalloc(1, sizeof(*p));
  p->id = id;
  p->name = xstrdup(name);
  p->next_track_id = 1;
  return p;
}

void playlist_free(Playlist *p) {
  if (!p) {
    return;
  }
  for (size_t i = 0; i < p->len; i++) {
    track_free(&p->tracks[i]);
  }
  free(p->tracks);
  free(p->name);
  free(p);
}

ssize_t playlist_index(const Playlist *p, uint32_t track_id) {
  for (size_t i = 0; i < p->len; i++) {
    if (p->tracks[i].id == track_id) {
      return (ssize_t)i;
    }
  }
  return -1;
}

size_t playlist_insert(Playlist *p, size_t pos, Track *tracks, size_t n) {
  if (pos > p->len) {
    pos = p->len;
  }
  if (n == 0) {
    return pos;
  }
  VEC_RESERVE(p->tracks, p->len, p->cap, n);
  memmove(p->tracks + pos + n, p->tracks + pos, (p->len - pos) * sizeof(Track));
  for (size_t i = 0; i < n; i++) {
    if (tracks[i].id == 0 || tracks[i].id >= p->next_track_id) {
      tracks[i].id = p->next_track_id++;
    }
    p->tracks[pos + i] = tracks[i];
  }
  p->len += n;
  p->dirty = true;
  return pos;
}

static bool id_in(uint32_t id, const uint32_t *ids, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (ids[i] == id) {
      return true;
    }
  }
  return false;
}

size_t playlist_remove(Playlist *p, const uint32_t *ids, size_t n) {
  size_t w = 0;
  for (size_t r = 0; r < p->len; r++) {
    if (id_in(p->tracks[r].id, ids, n)) {
      track_free(&p->tracks[r]);
    } else {
      p->tracks[w++] = p->tracks[r];
    }
  }
  size_t removed = p->len - w;
  p->len = w;
  if (removed) {
    p->dirty = true;
  }
  return removed;
}

size_t playlist_move(Playlist *p, size_t pos, const uint32_t *ids, size_t n) {
  Track *block = xmalloc((n ? n : 1) * sizeof(Track));
  size_t nb = 0;
  size_t w = 0;
  for (size_t r = 0; r < p->len; r++) {
    if (nb < n && id_in(p->tracks[r].id, ids, n)) {
      block[nb++] = p->tracks[r];
    } else {
      p->tracks[w++] = p->tracks[r];
    }
  }
  p->len = w;
  if (pos > p->len) {
    pos = p->len;
  }
  memmove(p->tracks + pos + nb, p->tracks + pos, (p->len - pos) * sizeof(Track));
  memcpy(p->tracks + pos, block, nb * sizeof(Track));
  p->len += nb;
  free(block);
  if (nb) {
    p->dirty = true;
  }
  return nb;
}

void playlist_rename(Playlist *p, const char *name) {
  free(p->name);
  p->name = xstrdup(name);
  p->dirty = true;
}

void model_init(void) {
  memset(&M, 0, sizeof(M));
  M.next_pl_id = 1;
  M.order = ORDER_SEQ;
  M.volume = 100;
}

void model_free(void) {
  for (size_t i = 0; i < M.len; i++) {
    playlist_free(M.pls[i]);
  }
  free(M.pls);
  free(M.library);
  free(M.output);
  memset(&M, 0, sizeof(M));
}

ssize_t model_index(uint32_t id) {
  for (size_t i = 0; i < M.len; i++) {
    if (M.pls[i]->id == id) {
      return (ssize_t)i;
    }
  }
  return -1;
}

Playlist *model_find(uint32_t id) {
  ssize_t i = model_index(id);
  return i < 0 ? NULL : M.pls[i];
}

void model_add(Playlist *p) {
  if (p->id >= M.next_pl_id) {
    M.next_pl_id = p->id + 1;
  }
  VEC_PUSH(M.pls, M.len, M.cap, p);
  M.list_dirty = true;
}

Playlist *model_create(const char *name) {
  Playlist *p = playlist_new(M.next_pl_id++, name);
  p->dirty = true;
  model_add(p);
  return p;
}

bool model_delete(uint32_t id) {
  ssize_t i = model_index(id);
  if (i < 0) {
    return false;
  }
  playlist_free(M.pls[i]);
  memmove(M.pls + i, M.pls + i + 1, (M.len - (size_t)i - 1) * sizeof(Playlist *));
  M.len--;
  M.list_dirty = true;
  return true;
}

bool model_move(uint32_t id, size_t pos) {
  ssize_t i = model_index(id);
  if (i < 0) {
    return false;
  }
  Playlist *p = M.pls[i];
  memmove(M.pls + i, M.pls + i + 1, (M.len - (size_t)i - 1) * sizeof(Playlist *));
  M.len--;
  if (pos > M.len) {
    pos = M.len;
  }
  memmove(M.pls + pos + 1, M.pls + pos, (M.len - pos) * sizeof(Playlist *));
  M.pls[pos] = p;
  M.len++;
  M.list_dirty = true;
  return true;
}

Order model_effective_order(const Playlist *p) {
  return (p && p->order != ORDER_INHERIT) ? p->order : M.order;
}
