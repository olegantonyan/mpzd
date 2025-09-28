#include "rpc/server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_EVENTS 64

static bool set_nonblocking(int fd);
static void handle_client_data(const ServerHandle_t *handle, int client_fd);
static void handle_new_connection(const ServerHandle_t *handle);

bool server_init(ServerHandle_t *out_handle, unsigned short port, ServerCallbackFunction_t callback) {
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd == 0) {
    perror("socket(AF_INET, SOCK_STREAM, 0)");
    return false;
  }

  int opt = 1;
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
    perror("setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))");
    close(server_fd);
    return false;
  }

  if (set_nonblocking(server_fd) < 0) {
    close(server_fd);
    return false;
  }

  struct sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(port);
  if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    perror("bind(server_fd, (struct sockaddr *)&address, sizeof(address)");
    close(server_fd);
    return false;
  }

  if (listen(server_fd, SOMAXCONN) < 0) {
    perror("listen(server_fd, SOMAXCONN)");
    close(server_fd);
    return false;
  }

  int epoll_fd = epoll_create1(0);
  if (epoll_fd == -1) {
    perror("epoll_create1(0)");
    close(server_fd);
    close(epoll_fd);
    return false;
  }

  struct epoll_event event, events[MAX_EVENTS];
  event.events = EPOLLIN;
  event.data.fd = server_fd;

  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) < 0) {
    perror("epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event)");
    close(server_fd);
    close(epoll_fd);
    return false;
  }

  out_handle->epoll_fd = epoll_fd;
  out_handle->server_fd = server_fd;
  out_handle->callback = callback;

  return true;
}

bool server_poll(const ServerHandle_t *handle) {
  struct epoll_event events[MAX_EVENTS];

  int nfds = epoll_wait(handle->epoll_fd, events, MAX_EVENTS, -1);
  if (nfds == -1) {
    perror("epoll_wait(handle->epoll_fd, events, MAX_EVENTS, -1)");
    return false;
  }

  for (int i = 0; i < nfds; i++) {
    if (events[i].data.fd == handle->server_fd) {
      handle_new_connection(handle);
    } else {
      handle_client_data(handle, events[i].data.fd);
    }
  }

  return true;
}

static bool set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    perror("fcntl(fd, F_GETFL, 0)");
    return false;
  }

  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    perror("fcntl(fd, F_SETFL, flags | O_NONBLOCK)");
    return false;
  }
  return true;
}

static void handle_new_connection(const ServerHandle_t *handle) {
  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);
  int client_fd;
  struct epoll_event event;

  while ((client_fd = accept(handle->server_fd, (struct sockaddr*)&client_addr, &client_len)) > 0) {
    if (!set_nonblocking(client_fd)) {
      close(client_fd);
      continue;
    }

    event.events = EPOLLIN | EPOLLET;
    event.data.fd = client_fd;

    if (epoll_ctl(handle->epoll_fd, EPOLL_CTL_ADD, client_fd, &event) < 0) {
      perror("epoll_ctl(handle->epoll_fd, EPOLL_CTL_ADD, client_fd, &event)");
      close(client_fd);
      continue;
    }
  }

  if (client_fd < 0 && (errno != EAGAIN && errno != EWOULDBLOCK)) {
    perror("accept");
  }
}

static void handle_client_data(const ServerHandle_t *handle, int client_fd) {
  unsigned char buffer[50000] = { 0 };
  ssize_t bytes_read;

  while (true) {
    ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

    if (bytes_read > 0) {
      int bytes_to_send = handle->callback(client_fd, buffer, sizeof(buffer), bytes_read);

      if (bytes_to_send > 0) {
        if (send(client_fd, buffer, bytes_to_send, 0) < 0) {
          perror("send(client_fd, buffer, bytes_to_send, 0)");
          break;
        }
      } else {
        // epoll_ctl(handle->epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
        // close(client_fd);
        break;
      }
    } else if (bytes_read == 0) {
      epoll_ctl(handle->epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
      close(client_fd);
      break;
    } else {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("recv(client_fd, buffer, sizeof(buffer) - 1, 0)");
        epoll_ctl(handle->epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
        close(client_fd);
      }
      break;
    }
  }
}
