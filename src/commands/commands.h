#ifndef COMMANDS_COMMANDS_H
#define COMMANDS_COMMANDS_H

#include "mediaplayer/mediaplayer.h"

#include <stdbool.h>

typedef struct {
  MediaPlayerHandle_t mp;
} CommandsContext_t;

void commands_init(CommandsContext_t *context);

#endif
