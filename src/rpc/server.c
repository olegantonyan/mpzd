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

static const char* get_last_error();
static ServerResult_t server_poll(const ServerHandle_t *handle);
static int set_nonblocking(int fd);
static void handle_client_data(const ServerHandle_t *handle, int client_fd);
static void handle_new_connection(const ServerHandle_t *handle);

ServerResult_t server_init(ServerHandle_t *out_handle, int port, ServerCallbackFunction_t callback) {
  ServerResult_t result = { .ok = true, .error_string = NULL };

  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd == 0) {
    result.ok = false;
    result.error_string = get_last_error();
    return result;
  }

  int opt = 1;
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
    result.ok = false;
    result.error_string = get_last_error();
    close(server_fd);
    return result;
  }

  if (set_nonblocking(server_fd) < 0) {
    result.ok = false;
    result.error_string = get_last_error();
    close(server_fd);
    return result;
  }

  struct sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(port);
  if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    result.ok = false;
    result.error_string = get_last_error();
    close(server_fd);
    return result;
  }

  if (listen(server_fd, SOMAXCONN) < 0) {
    result.ok = false;
    result.error_string = get_last_error();
    close(server_fd);
    return result;
  }

  int epoll_fd = epoll_create1(0);
  if (epoll_fd == -1) {
    result.ok = false;
    result.error_string = get_last_error();
    close(server_fd);
    close(epoll_fd);
    return result;
  }

  struct epoll_event event, events[MAX_EVENTS];
  event.events = EPOLLIN;
  event.data.fd = server_fd;

  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) == -1) {
    result.ok = false;
    result.error_string = get_last_error();
    close(server_fd);
    close(epoll_fd);
    return result;
  }

  out_handle->epoll_fd = epoll_fd;
  out_handle->server_fd = server_fd;
  out_handle->poll_func = (ServerResult_t (*)(const void *))server_poll;
  out_handle->callback = callback;

  return result;
}

int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    return -1;
  }

  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    return -1;
  }
  return 0;
}

static ServerResult_t server_poll(const ServerHandle_t *handle) {
  ServerResult_t result = { .ok = true, .error_string = NULL };
  struct epoll_event events[MAX_EVENTS];

  int nfds = epoll_wait(handle->epoll_fd, events, MAX_EVENTS, -1);
  if (nfds == -1) {
    result.ok = false;
    result.error_string = get_last_error();
    return result;
  }

  for (int i = 0; i < nfds; i++) {
    if (events[i].data.fd == handle->server_fd) {
      handle_new_connection(handle);
    } else {
      handle_client_data(handle, events[i].data.fd);
    }
  }

  return result;
}

void handle_new_connection(const ServerHandle_t *handle) {
  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);
  int client_fd;
  struct epoll_event event;

  while ((client_fd = accept(handle->server_fd, (struct sockaddr*)&client_addr, &client_len)) > 0) {
    if (set_nonblocking(client_fd) == -1) {
      close(client_fd);
      continue;
    }

    event.events = EPOLLIN | EPOLLET;
    event.data.fd = client_fd;

    if (epoll_ctl(handle->epoll_fd, EPOLL_CTL_ADD, client_fd, &event) == -1) {
      close(client_fd);
      continue;
    }
  }

  //if (client_fd == -1 && (errno != EAGAIN && errno != EWOULDBLOCK)) {
  //  perror("accept");
  //}
}

void handle_client_data(const ServerHandle_t *handle, int client_fd) {
  char buffer[100000] = { 0 };
  ssize_t bytes_read;

  while (true) {
    ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

    if (bytes_read > 0) {
      int bytes_to_send;
      void *send_buffer;
      handle->callback(client_fd, buffer, bytes_read, &send_buffer, &bytes_to_send);

      if (bytes_to_send > 0) {
        send(client_fd, send_buffer, bytes_to_send, 0);
      }
    } else if (bytes_read == 0) {
      epoll_ctl(handle->epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
      close(client_fd);
      break;
    } else {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        //perror("recv");
        epoll_ctl(handle->epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
        close(client_fd);
      }
      break;
    }
  }
}

static const char* get_last_error() {
    static __thread char buf[1024] = {0};
    int err = errno;

#if ((_POSIX_C_SOURCE >= 200112L) && !defined(_GNU_SOURCE))
    if (strerror_r(err, buf, sizeof(buf)) != 0) {
      snprintf(buf, sizeof(buf), "Unknown error %d", err);
    }
    return buf;
#else
    char *msg = strerror_r(err, buf, sizeof(buf));
    return msg;
#endif
}
