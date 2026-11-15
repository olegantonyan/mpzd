#ifndef PLAYER_OUTPUTS_H
#define PLAYER_OUTPUTS_H

#include "util/util.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
  char *id;
  char *desc;
} Output;

typedef struct {
  Output *v;
  size_t len;
  size_t cap;
} OutputList;

void output_list_push(OutputList *l, const char *id, const char *desc);
void output_list_free(OutputList *l);

bool outputs_init(bool pipewire, const StrVec *specs, const char *device);
void outputs_free(void);
bool outputs_list(OutputList *out);
bool outputs_valid(const char *id);
char *outputs_pcm(const char *id);

#endif
