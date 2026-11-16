#ifndef PLAYER_SINK_H
#define PLAYER_SINK_H

#include "player/pcm.h"

#include <stdbool.h>
#include <stdint.h>

bool sink_open(const char *device, PcmFmt f);
bool sink_prepare(void);
void sink_drop(void);
void sink_close(void);
int sink_write(const uint8_t *data, int frames);
bool sink_wait(int timeout_ms);
int64_t sink_delay(void);
bool sink_running(void);
bool sink_pause(bool on);
bool sink_drain(void);
bool sink_draining(void);

#endif
