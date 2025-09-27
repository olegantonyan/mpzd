#ifndef RPC_SERVER_H
#define RPC_SERVER_H

#include <stdbool.h>

typedef int (*ServerCallbackFunction_t)(int fd, void *buffer, int buffer_size, int recv_size);

typedef struct {
  int server_fd;
  int epoll_fd;
  bool (*poll_func)(const void *this);
  ServerCallbackFunction_t callback;
} ServerHandle_t;

bool server_init(ServerHandle_t *out_handle, int port, ServerCallbackFunction_t callback);

#endif
