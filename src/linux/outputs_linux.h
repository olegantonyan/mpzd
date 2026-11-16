#ifndef LINUX_OUTPUTS_LINUX_H
#define LINUX_OUTPUTS_LINUX_H

#include "player/outputs.h"
#include "util/util.h"

#include <stdbool.h>

bool outputs_init(bool pipewire, const StrVec *specs, const char *device);
void outputs_free(void);

#endif
