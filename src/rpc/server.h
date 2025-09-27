#ifndef RPC_SERVER_H
#define RPC_SERVER_H

#include <stdbool.h>

typedef struct {
  bool ok;
  const char *error_string;
} ServerResult_t;

typedef void (*ServerCallbackFunction_t)(int fd, const void *recv, int recv_size, void **send_buffer, int *send_bytes);

typedef struct {
  int server_fd;
  int epoll_fd;
  ServerResult_t (*poll_func)(const void *this);
  ServerCallbackFunction_t callback;
} ServerHandle_t;

ServerResult_t server_init(ServerHandle_t *out_handle, int port, ServerCallbackFunction_t callback);

#endif
