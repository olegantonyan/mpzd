#ifndef RPC_SERVER_H
#define RPC_SERVER_H

#include <stdbool.h>

typedef struct {
  bool ok;
  const char *error_string;
} ServerResult_t;

typedef struct {
  int server_fd;
  int epoll_fd;
  ServerResult_t (*poll_func)(const void *this);
} ServerHandle_t;

ServerResult_t server_init(ServerHandle_t *out_handle, int port);

#endif
