#include "player/outputs.h"

#include <stdlib.h>
#include <string.h>

void output_list_push(OutputList *l, const char *id, const char *desc) {
  Output o = { xstrdup(id), xstrdup(desc && *desc ? desc : id) };
  VEC_PUSH(l->v, l->len, l->cap, o);
}

void output_list_free(OutputList *l) {
  for (size_t i = 0; i < l->len; i++) {
    free(l->v[i].id);
    free(l->v[i].desc);
  }
  free(l->v);
  memset(l, 0, sizeof(*l));
}
