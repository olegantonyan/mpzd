#ifndef NET_SERVER_H
#define NET_SERVER_H

#include <stdbool.h>
#include <stddef.h>

#define SERVER_LINE_MAX 8192

typedef struct Client Client;
typedef void (*LineHandler)(Client *c, char *line);
typedef void (*FdHandler)(void *ud);

typedef struct {
  const char *bind;
  const char *port;
  const char *socket_path;
  const char *greeting;
  int max_clients;
  size_t max_output;
} ServerConfig;

bool server_init(const ServerConfig *cfg, LineHandler on_line);
bool server_add_fd(int fd, FdHandler cb, void *ud);
void server_run(void);
void server_stop(void);
void server_shutdown(void);

void client_send(Client *c, const char *data, size_t n);
void client_set_watch(Client *c, bool on);
void client_close_later(Client *c);

void server_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void server_flush_events(void);

#endif
