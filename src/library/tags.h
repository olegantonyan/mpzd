#ifndef LIBRARY_TAGS_H
#define LIBRARY_TAGS_H

#include "model/track.h"

#include <stdbool.h>
#include <stdint.h>

bool tags_read(const char *path, Track *out);
uint32_t tags_duration_ms(const char *path);

#endif
