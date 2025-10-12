#ifndef COMMANDS_COMMANDS_H
#define COMMANDS_COMMANDS_H

#include "mediaplayer/mediaplayer.h"
#include "rpc/server.h"

#include <stdbool.h>

typedef struct {
  MediaPlayerHandle_t *mp;
  ServerHandle_t *server;
} CommandsContext_t;

bool commands_init(const CommandsContext_t *context);

#endif
