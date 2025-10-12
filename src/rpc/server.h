#ifndef RPC_SERVER_H
#define RPC_SERVER_H

#include <stdbool.h>
#include <unistd.h>

typedef ssize_t (*ServerCallbackFunction_t)(const void *server_handle, int fd, void *buffer, ssize_t buffer_size, ssize_t recv_size);

typedef struct {
  int server_fd;
  int epoll_fd;
  ServerCallbackFunction_t callback;
  unsigned short port;
} ServerHandle_t;

bool server_init(ServerHandle_t *out_handle, unsigned short port, ServerCallbackFunction_t callback);
bool server_poll(const ServerHandle_t *handle);

#endif
