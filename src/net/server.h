#ifndef NET_SERVER_H
#define NET_SERVER_H

#include <stdbool.h>
#include <stddef.h>

#define SERVER_LINE_MAX 8192

typedef struct Client Client;
typedef void (*LineHandler)(Client *c, char *line);

typedef struct {
  const char *bind;
  const char *port;
  const char *greeting;
  int max_clients;
  size_t max_output;
} ServerConfig;

bool server_init(const ServerConfig *cfg, LineHandler on_line);
bool server_listen_fd(int fd, bool tcp);
void server_shutdown(void);

void client_send(Client *c, const char *data, size_t n);
void client_set_watch(Client *c, bool on);
void client_close_later(Client *c);
void client_hold(Client *c);
void client_release(Client *c);
bool client_alive(const Client *c);

void server_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void server_notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void server_flush_events(void);

#endif
