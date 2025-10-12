#include "mediaplayer/mediaplayer.h"
#include "rpc/server.h"
#include "rpc/protocol.h"
#include "commands/commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#define LOG_MODULE "[main] "

static ssize_t server_callback(const void *server, int fd, void *buffer, ssize_t buffer_size, ssize_t recv_size);

int main(int argc, char **argv) {
  ServerHandle_t server;
  if (!server_init(&server, 6601, server_callback)) {
    fprintf(stderr, LOG_MODULE"error initilizing server");
    return 1;
  }

  MediaPlayerHandle_t mp;
  MediaPlayerResult_t init_res = mediaplayer_init(&mp);
  if (!init_res.ok) {
    fprintf(stderr, LOG_MODULE"%s error initializing mediaplayer: %s\n", init_res.error_string);
    return 1;
  }

  CommandsContext_t ctx = { .mp = &mp };
  commands_init(&ctx);

  while (true) {
    server_poll(&server);
  }

  return 0;
}

static ssize_t server_callback(const void *server, int fd, void *buffer, ssize_t buffer_size, ssize_t recv_size) {
  ServerHandle_t *server_handle = (ServerHandle_t *)server;
  ssize_t output_size = 0;
  if (!protocol_handle(buffer, buffer_size, recv_size, &output_size)) {
    fprintf(stderr, LOG_MODULE"error processing command\n");
  }
  return output_size;
}
