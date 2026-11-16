#include "net/server.h"
#include "util/loop.h"
#include "util/util.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define LOG_MODULE "server"
#define MAX_LISTENERS 8

typedef struct {
  int fd;
  bool tcp;
  LoopFd *lh;
} Listener;

struct Client {
  int fd;
  LoopFd *lh;
  char in[SERVER_LINE_MAX + 1];
  size_t inlen;
  bool discarding;
  StrBuf out;
  bool watch;
  bool closing;
  bool dead;
  bool want_out;
  int held;
  Client *next;
};

static struct {
  ServerConfig cfg;
  LineHandler on_line;
  Listener listeners[MAX_LISTENERS];
  int nlisteners;
  Client *clients;
  int nclients;
  StrVec events;
} SV;

static bool set_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

static void accept_clients(void *ud, short revents);

bool server_listen_fd(int fd, bool tcp) {
  if (SV.nlisteners >= MAX_LISTENERS || !set_nonblock(fd) || listen(fd, 16) != 0) {
    close(fd);
    return false;
  }
  Listener *l = &SV.listeners[SV.nlisteners++];
  l->fd = fd;
  l->tcp = tcp;
  l->lh = loop_add_fd(fd, POLLIN, accept_clients, l);
  return true;
}

static bool listen_tcp(const char *host, const char *port) {
  struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE };
  struct addrinfo *res = NULL;
  int rc = getaddrinfo(host, port, &hints, &res);
  if (rc != 0) {
    LOG("getaddrinfo %s:%s: %s", host ? host : "*", port, gai_strerror(rc));
    return false;
  }
  bool any = false;
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
      continue;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (ai->ai_family == AF_INET6) {
      setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
    }
    if (bind(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
      LOG("bind %s:%s: %s", host ? host : "*", port, strerror(errno));
      close(fd);
      continue;
    }
    if (server_listen_fd(fd, true)) {
      any = true;
    }
  }
  freeaddrinfo(res);
  if (any) {
    LOG("listening on %s:%s", host ? host : "*", port);
  }
  return any;
}

static void reap_clients(void);

bool server_init(const ServerConfig *cfg, LineHandler on_line) {
  SV.cfg = *cfg;
  SV.on_line = on_line;
  loop_set_hooks(server_flush_events, reap_clients);
  return cfg->port && *cfg->port && listen_tcp(cfg->bind && *cfg->bind ? cfg->bind : NULL, cfg->port);
}

static void client_free(Client *c) {
  loop_del(c->lh);
  close(c->fd);
  sb_free(&c->out);
  free(c);
}

static void reap_clients(void) {
  Client **pp = &SV.clients;
  while (*pp) {
    Client *c = *pp;
    if (!c->held && (c->dead || (c->closing && c->out.len == 0))) {
      *pp = c->next;
      SV.nclients--;
      client_free(c);
    } else {
      pp = &c->next;
    }
  }
}

static void update_interest(Client *c) {
  if (!c->dead) {
    loop_mod(c->lh, (short)((c->held ? 0 : POLLIN) | (c->want_out ? POLLOUT : 0)));
  }
}

static void client_flush(Client *c) {
  while (c->out.len && !c->dead) {
    ssize_t n = send(c->fd, c->out.data, c->out.len, MSG_NOSIGNAL);
    if (n > 0) {
      sb_consume(&c->out, (size_t)n);
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      c->dead = true;
    }
  }
  bool want = c->out.len > 0 && !c->dead;
  if (want != c->want_out) {
    c->want_out = want;
    update_interest(c);
  }
}

void client_send(Client *c, const char *data, size_t n) {
  if (c->dead) {
    return;
  }
  sb_append(&c->out, data, n);
  if (c->out.len > SV.cfg.max_output) {
    LOG("client %d output overflow, disconnecting", c->fd);
    c->dead = true;
    return;
  }
  client_flush(c);
}

void client_set_watch(Client *c, bool on) {
  c->watch = on;
}

void client_close_later(Client *c) {
  c->closing = true;
}

static void process_lines(Client *c);

void client_hold(Client *c) {
  if (c->held++ == 0) {
    update_interest(c);
  }
}

void client_release(Client *c) {
  if (--c->held > 0) {
    return;
  }
  update_interest(c);
  if (!c->dead && !c->closing) {
    process_lines(c);
  }
}

bool client_alive(const Client *c) {
  return !c->dead;
}

static void queue_event(bool dedup, const char *fmt, va_list ap) {
  StrBuf sb = { 0 };
  sb_puts(&sb, "! ");
  sb_vprintf(&sb, fmt, ap);
  sb_puts(&sb, "\n");
  for (size_t i = 0; dedup && i < SV.events.len; i++) {
    if (strcmp(SV.events.v[i], sb.data) == 0) {
      sb_free(&sb);
      return;
    }
  }
  strvec_push(&SV.events, sb.data);
}

void server_event(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  queue_event(true, fmt, ap);
  va_end(ap);
}

void server_notify(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  queue_event(false, fmt, ap);
  va_end(ap);
}

void server_flush_events(void) {
  for (size_t i = 0; i < SV.events.len; i++) {
    size_t n = strlen(SV.events.v[i]);
    for (Client *c = SV.clients; c; c = c->next) {
      if (c->watch && !c->closing) {
        client_send(c, SV.events.v[i], n);
      }
    }
  }
  strvec_free(&SV.events);
}

static void set_keepalive(int fd) {
  int one = 1, idle = 15, intvl = 5, cnt = 3;
  bool ok = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one)) == 0 && setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) == 0 && setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl)) == 0 && setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt)) == 0;
#ifdef TCP_USER_TIMEOUT
  unsigned user_timeout = 30000;
  ok = ok && setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout, sizeof(user_timeout)) == 0;
#endif
  if (!ok) {
    LOG("keepalive: %s", strerror(errno));
  }
}

static void on_client(void *ud, short revents);

static void accept_clients(void *ud, short revents) {
  Listener *l = ud;
  for (;;) {
    int fd = accept(l->fd, NULL, NULL);
    if (fd < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        LOG("accept: %s", strerror(errno));
      }
      return;
    }
    if (SV.nclients >= SV.cfg.max_clients) {
      static const char msg[] = "err 5 too many clients\n";
      if (send(fd, msg, sizeof(msg) - 1, MSG_NOSIGNAL) < 0) {
        LOG("send: %s", strerror(errno));
      }
      close(fd);
      continue;
    }
    if (!set_nonblock(fd)) {
      close(fd);
      continue;
    }
    if (l->tcp) {
      set_keepalive(fd);
    }
    Client *c = xcalloc(1, sizeof(*c));
    c->fd = fd;
    c->lh = loop_add_fd(fd, POLLIN, on_client, c);
    c->next = SV.clients;
    SV.clients = c;
    SV.nclients++;
    if (SV.cfg.greeting) {
      client_send(c, SV.cfg.greeting, strlen(SV.cfg.greeting));
    }
  }
}

static void process_lines(Client *c) {
  size_t start = 0;
  for (size_t i = 0; i < c->inlen && !c->closing && !c->dead && !c->held; i++) {
    if (c->in[i] != '\n') {
      continue;
    }
    c->in[i] = '\0';
    char *line = c->in + start;
    size_t len = i - start;
    if (len && line[len - 1] == '\r') {
      line[len - 1] = '\0';
    }
    start = i + 1;
    if (c->discarding) {
      c->discarding = false;
      static const char msg[] = "err 1 line too long\n";
      client_send(c, msg, sizeof(msg) - 1);
    } else {
      SV.on_line(c, line);
    }
    server_flush_events();
  }
  if (c->closing) {
    c->inlen = 0;
    return;
  }
  memmove(c->in, c->in + start, c->inlen - start);
  c->inlen -= start;
  if (c->inlen == SERVER_LINE_MAX && !c->held) {
    c->discarding = true;
    c->inlen = 0;
  }
}

static void read_client(Client *c) {
  ssize_t n = recv(c->fd, c->in + c->inlen, SERVER_LINE_MAX - c->inlen, 0);
  if (n == 0) {
    c->dead = true;
    return;
  }
  if (n < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      c->dead = true;
    }
    return;
  }
  c->inlen += (size_t)n;
  process_lines(c);
}

static void on_client(void *ud, short revents) {
  Client *c = ud;
  if (c->dead) {
    return;
  }
  if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
    c->dead = true;
    return;
  }
  if (revents & POLLOUT) {
    client_flush(c);
  }
  if (revents & POLLIN) {
    read_client(c);
  }
}

void server_shutdown(void) {
  for (Client *c = SV.clients; c; c = c->next) {
    c->dead = true;
    c->held = 0;
  }
  reap_clients();
  for (int i = 0; i < SV.nlisteners; i++) {
    loop_del(SV.listeners[i].lh);
    close(SV.listeners[i].fd);
  }
  SV.nlisteners = 0;
  strvec_free(&SV.events);
}
