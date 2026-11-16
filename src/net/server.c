#include "net/server.h"
#include "util/util.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define LOG_MODULE "server"
#define MAX_EVENTS 32
#define MAX_LISTENERS 8

typedef enum { H_LISTEN, H_CLIENT, H_FD } HandleType;

typedef struct {
  HandleType type;
  int fd;
  bool tcp;
} Handle;

typedef struct {
  Handle h;
  FdHandler cb;
  void *ud;
} ExtFd;

struct Client {
  Handle h;
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
  int epfd;
  ServerConfig cfg;
  LineHandler on_line;
  Handle listeners[MAX_LISTENERS];
  int nlisteners;
  ExtFd *ext[8];
  int next_ext;
  Client *clients;
  int nclients;
  StrVec events;
  bool stop;
} SV = { .epfd = -1 };

static bool epoll_add(int fd, uint32_t events, void *ptr) {
  struct epoll_event ev = { .events = events, .data.ptr = ptr };
  return epoll_ctl(SV.epfd, EPOLL_CTL_ADD, fd, &ev) == 0;
}

static void epoll_mod(int fd, uint32_t events, void *ptr) {
  struct epoll_event ev = { .events = events, .data.ptr = ptr };
  epoll_ctl(SV.epfd, EPOLL_CTL_MOD, fd, &ev);
}

static bool add_listener(int fd, bool tcp) {
  if (SV.nlisteners >= MAX_LISTENERS || listen(fd, 16) != 0) {
    close(fd);
    return false;
  }
  Handle *h = &SV.listeners[SV.nlisteners++];
  h->type = H_LISTEN;
  h->fd = fd;
  h->tcp = tcp;
  return epoll_add(fd, EPOLLIN, h);
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
    int fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, ai->ai_protocol);
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
    if (add_listener(fd, true)) {
      any = true;
    }
  }
  freeaddrinfo(res);
  if (any) {
    LOG("listening on %s:%s", host ? host : "*", port);
  }
  return any;
}

static bool listen_unix(const char *path) {
  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  if (strlen(path) >= sizeof(addr.sun_path)) {
    LOG("socket path too long: %s", path);
    return false;
  }
  strcpy(addr.sun_path, path);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return false;
  }
  unlink(path);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    LOG("bind %s: %s", path, strerror(errno));
    close(fd);
    return false;
  }
  LOG("listening on %s", path);
  return add_listener(fd, false);
}

bool server_init(const ServerConfig *cfg, LineHandler on_line) {
  SV.cfg = *cfg;
  SV.on_line = on_line;
  SV.epfd = epoll_create1(EPOLL_CLOEXEC);
  if (SV.epfd < 0) {
    LOG("epoll_create1: %s", strerror(errno));
    return false;
  }
  bool ok = false;
  if (cfg->port && *cfg->port) {
    ok = listen_tcp(cfg->bind && *cfg->bind ? cfg->bind : NULL, cfg->port) || ok;
  }
  if (cfg->socket_path && *cfg->socket_path) {
    ok = listen_unix(cfg->socket_path) || ok;
  }
  return ok;
}

bool server_add_fd(int fd, FdHandler cb, void *ud) {
  if (SV.next_ext >= (int)ARRAY_LEN(SV.ext)) {
    return false;
  }
  ExtFd *e = xcalloc(1, sizeof(*e));
  e->h.type = H_FD;
  e->h.fd = fd;
  e->cb = cb;
  e->ud = ud;
  SV.ext[SV.next_ext++] = e;
  return epoll_add(fd, EPOLLIN, e);
}

static void client_free(Client *c) {
  epoll_ctl(SV.epfd, EPOLL_CTL_DEL, c->h.fd, NULL);
  close(c->h.fd);
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
    epoll_mod(c->h.fd, (c->held ? 0 : EPOLLIN) | (c->want_out ? EPOLLOUT : 0), c);
  }
}

static void client_flush(Client *c) {
  while (c->out.len && !c->dead) {
    ssize_t n = send(c->h.fd, c->out.data, c->out.len, MSG_NOSIGNAL);
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
    LOG("client %d output overflow, disconnecting", c->h.fd);
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
  unsigned user_timeout = 30000;
  if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one)) != 0 || setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) != 0 || setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl)) != 0 || setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt)) != 0 || setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout, sizeof(user_timeout)) != 0) {
    LOG("keepalive: %s", strerror(errno));
  }
}

static void accept_clients(Handle *l) {
  for (;;) {
    int fd = accept4(l->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
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
    if (l->tcp) {
      set_keepalive(fd);
    }
    Client *c = xcalloc(1, sizeof(*c));
    c->h.type = H_CLIENT;
    c->h.fd = fd;
    if (!epoll_add(fd, EPOLLIN, c)) {
      close(fd);
      free(c);
      continue;
    }
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
  ssize_t n = recv(c->h.fd, c->in + c->inlen, SERVER_LINE_MAX - c->inlen, 0);
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

void server_run(void) {
  struct epoll_event events[MAX_EVENTS];
  while (!SV.stop) {
    int n = epoll_wait(SV.epfd, events, MAX_EVENTS, -1);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      LOG("epoll_wait: %s", strerror(errno));
      break;
    }
    for (int i = 0; i < n; i++) {
      Handle *h = events[i].data.ptr;
      switch (h->type) {
        case H_LISTEN:
          accept_clients(h);
          break;
        case H_FD: {
          ExtFd *e = (ExtFd *)h;
          e->cb(e->ud);
          break;
        }
        case H_CLIENT: {
          Client *c = (Client *)h;
          if (c->dead) {
            break;
          }
          if (events[i].events & (EPOLLERR | EPOLLHUP)) {
            c->dead = true;
            break;
          }
          if (events[i].events & EPOLLOUT) {
            client_flush(c);
          }
          if (events[i].events & EPOLLIN) {
            read_client(c);
          }
          break;
        }
      }
      server_flush_events();
    }
    reap_clients();
  }
}

void server_stop(void) {
  SV.stop = true;
}

void server_shutdown(void) {
  for (Client *c = SV.clients; c; c = c->next) {
    c->dead = true;
    c->held = 0;
  }
  reap_clients();
  for (int i = 0; i < SV.nlisteners; i++) {
    close(SV.listeners[i].fd);
  }
  SV.nlisteners = 0;
  if (SV.cfg.socket_path && *SV.cfg.socket_path) {
    unlink(SV.cfg.socket_path);
  }
  for (int i = 0; i < SV.next_ext; i++) {
    free(SV.ext[i]);
  }
  SV.next_ext = 0;
  strvec_free(&SV.events);
  if (SV.epfd >= 0) {
    close(SV.epfd);
    SV.epfd = -1;
  }
}
