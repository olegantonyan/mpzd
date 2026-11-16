#ifndef LINUX_PW_SINKS_H
#define LINUX_PW_SINKS_H

#include "player/outputs.h"

#ifdef MPZD_PIPEWIRE
void pw_sinks_init(void);
void pw_sinks_deinit(void);
bool pw_sinks_list(OutputList *out);
#else
static inline void pw_sinks_init(void) {}
static inline void pw_sinks_deinit(void) {}
static inline bool pw_sinks_list(OutputList *out) { return false; }
#endif

#endif
