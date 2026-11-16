#include "app.h"
#include "library/library.h"
#include "library/scan.h"
#include "linux/outputs_linux.h"
#include "model/model.h"
#include "net/server.h"
#include "player/audio.h"
#include "player/player.h"
#include "proto/handlers.h"
#include "proto/proto.h"
#include "store/store.h"
#include "util/bg.h"
#include "util/loop.h"
#include "util/util.h"
#include "version.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <libavutil/log.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define LOG_MODULE "main"
#define SAVE_DELAY_SEC 2
#define WATCHDOG_SEC 5
#define SHUTDOWN_SAVE_MS 5000

typedef struct {
  char *bind;
  char *port;
  char *socket;
  char *device;
  char *state_dir;
  char *max_clients;
  char *library;
  char *audio;
} Config;

static LoopTimer *save_timer;
static LoopTimer *watchdog_timer;
static bool save_armed;

static void usage(const char *prog) {
  printf("usage: %s [-c config] [-l library_dir] [-d state_dir] [-b bind_addr] [-p port] [-s unix_socket] [-a pipewire|alsa] [-o output]\n", prog);
}

static void set(char **dst, const char *v) {
  free(*dst);
  *dst = xstrdup(v);
}

static void overlay(Config *dst, const Config *src) {
  char **d = (char **)dst;
  char *const *s = (char *const *)src;
  for (size_t i = 0; i < sizeof(Config) / sizeof(char *); i++) {
    if (s[i]) {
      set(&d[i], s[i]);
    }
  }
}

static void config_free(Config *c) {
  char **f = (char **)c;
  for (size_t i = 0; i < sizeof(Config) / sizeof(char *); i++) {
    free(f[i]);
    f[i] = NULL;
  }
}

static bool config_load(const char *path, Config *c, StrVec *outputs) {
  FILE *f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
    return false;
  }
  char *line = NULL;
  size_t cap = 0;
  while (getline(&line, &cap, f) >= 0) {
    char *s = line;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '#' || !*s) {
      continue;
    }
    char *key = s;
    while (*s && !isspace((unsigned char)*s)) s++;
    if (*s) {
      *s++ = '\0';
    }
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) e--;
    *e = '\0';
    if (strcmp(key, "bind") == 0) set(&c->bind, s);
    else if (strcmp(key, "port") == 0) set(&c->port, s);
    else if (strcmp(key, "socket") == 0) set(&c->socket, s);
    else if (strcmp(key, "device") == 0) set(&c->device, s);
    else if (strcmp(key, "state_dir") == 0) set(&c->state_dir, s);
    else if (strcmp(key, "max_clients") == 0) set(&c->max_clients, s);
    else if (strcmp(key, "library") == 0) set(&c->library, s);
    else if (strcmp(key, "audio") == 0) set(&c->audio, s);
    else if (strcmp(key, "output") == 0) strvec_push(outputs, xstrdup(s));
    else fprintf(stderr, "%s: unknown key '%s'\n", path, key);
  }
  free(line);
  fclose(f);
  return true;
}

static char *default_state_dir(void) {
  StrBuf sb = { 0 };
  const char *xdg = getenv("XDG_STATE_HOME");
  const char *home = getenv("HOME");
  if (xdg && *xdg) {
    sb_printf(&sb, "%s/mpzd", xdg);
  } else if (home && *home) {
    sb_printf(&sb, "%s/.local/state/mpzd", home);
  } else {
    sb_puts(&sb, "/var/lib/mpzd");
  }
  return sb.data;
}

static void save_now(void) {
  StorePlaying playing = player_store_state();
  store_flush(&playing, false);
}

static void schedule_save(void) {
  if (save_armed || !save_timer) {
    return;
  }
  loop_timer_arm(save_timer, mono_ms() + SAVE_DELAY_SEC * 1000);
  save_armed = true;
}

static void on_save_timer(void *ud) {
  save_armed = false;
  save_now();
}

static void on_watchdog(void *ud) {
  loop_timer_arm(watchdog_timer, mono_ms() + WATCHDOG_SEC * 1000);
  scanner_watchdog();
  player_watchdog();
}

static bool listen_unix(const char *path) {
  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  if (strlen(path) >= sizeof(addr.sun_path)) {
    LOG("socket path too long: %s", path);
    return false;
  }
  strcpy(addr.sun_path, path);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
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
  return server_listen_fd(fd, false);
}

static void on_signal(void *ud, short revents) {
  int fd = *(int *)ud;
  struct signalfd_siginfo si;
  if (read(fd, &si, sizeof(si)) != sizeof(si)) {
    return;
  }
  if (si.ssi_signo == SIGHUP) {
    save_now();
  } else {
    LOG("signal %u, shutting down", si.ssi_signo);
    loop_stop();
  }
}

static void fix_selection(void) {
  Playlist *p = model_find(M.sel_pl);
  if (!p) {
    M.sel_pl = M.len ? M.pls[0]->id : 0;
    M.sel_track = 0;
  } else if (M.sel_track && playlist_index(p, M.sel_track) < 0) {
    M.sel_track = 0;
  }
}

int main(int argc, char **argv) {
  setlocale(LC_COLLATE, "");
  setlocale(LC_CTYPE, "");

  Config cli = { 0 };
  Config cfg = { 0 };
  const char *config_path = NULL;
  int opt;
  while ((opt = getopt(argc, argv, "c:l:d:b:p:s:a:o:hV")) != -1) {
    switch (opt) {
      case 'c': config_path = optarg; break;
      case 'l': set(&cli.library, optarg); break;
      case 'd': set(&cli.state_dir, optarg); break;
      case 'b': set(&cli.bind, optarg); break;
      case 'p': set(&cli.port, optarg); break;
      case 's': set(&cli.socket, optarg); break;
      case 'a': set(&cli.audio, optarg); break;
      case 'o': set(&cli.device, optarg); break;
      case 'V': printf("mpzd %s\n", PROJECT_VERSION); return 0;
      case 'h': usage(argv[0]); return 0;
      default: usage(argv[0]); return 2;
    }
  }

  set(&cfg.bind, "127.0.0.1");
  set(&cfg.port, "6601");
  set(&cfg.device, "default");
  set(&cfg.max_clients, "16");
#ifdef MPZD_PIPEWIRE
  set(&cfg.audio, "pipewire");
#else
  set(&cfg.audio, "alsa");
#endif
  char *state_dir = default_state_dir();
  set(&cfg.state_dir, state_dir);
  free(state_dir);
  StrVec outs = { 0 };
  if (config_path && !config_load(config_path, &cfg, &outs)) {
    return 1;
  }
  overlay(&cfg, &cli);
  config_free(&cli);
  if (!cfg.library) {
    fprintf(stderr, "no library folder configured (-l or library)\n");
    config_free(&cfg);
    strvec_free(&outs);
    return 1;
  }
  bool pipewire = strcmp(cfg.audio, "pipewire") == 0;
  if (!pipewire && strcmp(cfg.audio, "alsa") != 0) {
    fprintf(stderr, "audio must be alsa or pipewire\n");
    config_free(&cfg);
    strvec_free(&outs);
    return 1;
  }
  bool outputs_ok = outputs_init(pipewire, &outs, cfg.device);
  strvec_free(&outs);
  if (!outputs_ok) {
    fprintf(stderr, "mpzd is built without PipeWire support (-DMPZD_PIPEWIRE=ON)\n");
    config_free(&cfg);
    return 1;
  }

  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  sigaddset(&mask, SIGHUP);
  sigprocmask(SIG_BLOCK, &mask, NULL);
  signal(SIGPIPE, SIG_IGN);

  av_log_set_level(AV_LOG_QUIET);
  model_init();
  int lib_rc = library_set(cfg.library);
  if (lib_rc < 0) {
    LOG("library %s: %s", cfg.library, strerror(-lib_rc));
    model_free();
    config_free(&cfg);
    return 1;
  }
  if (!store_init(cfg.state_dir)) {
    return 1;
  }
  StorePlaying resume;
  if (!store_load(&resume)) {
    LOG("state in %s is unreadable, refusing to overwrite it", cfg.state_dir);
    return 1;
  }
  fix_selection();
  if (!M.output || !outputs_valid(M.output)) {
    free(M.output);
    M.output = xstrdup(cfg.device);
  }
  LOG("mpzd %s, library %s, output %s (%s), state %s, %zu playlists", PROJECT_VERSION, M.library, M.output, cfg.audio, cfg.state_dir, M.len);

  ServerConfig scfg = {
    .bind = cfg.bind,
    .port = cfg.port,
    .greeting = proto_greeting(),
    .max_clients = atoi(cfg.max_clients) > 0 ? atoi(cfg.max_clients) : 16,
    .max_output = 8 * 1024 * 1024,
  };
  bool listening = server_init(&scfg, proto_handle_line);
  if (cfg.socket && *cfg.socket) {
    listening = listen_unix(cfg.socket) || listening;
  }
  if (!listening) {
    LOG("no listening sockets");
    return 1;
  }

  int audio_fd, scan_fd;
  char *pcm = outputs_pcm(M.output);
  bool audio_ok = audio_init(pcm, &audio_fd);
  free(pcm);
  if (!audio_ok || !scanner_start(&scan_fd) || !bg_init()) {
    LOG("cannot start worker threads");
    return 1;
  }
  player_init(&resume);

  int sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  if (sig_fd < 0) {
    LOG("signalfd: %s", strerror(errno));
    return 1;
  }
  loop_add_fd(audio_fd, POLLIN, player_on_audio, NULL);
  loop_add_fd(scan_fd, POLLIN, scan_on_done, NULL);
  loop_add_fd(sig_fd, POLLIN, on_signal, &sig_fd);
  save_timer = loop_timer_new(on_save_timer, NULL);
  watchdog_timer = loop_timer_new(on_watchdog, NULL);
  loop_timer_arm(watchdog_timer, mono_ms() + WATCHDOG_SEC * 1000);
  app_on_change = schedule_save;

  loop_run();

  bg_shutdown();
  StorePlaying playing = player_store_state();
  store_flush(&playing, true);
  store_shutdown(SHUTDOWN_SAVE_MS);
  scanner_stop();
  audio_shutdown();
  server_shutdown();
  if (cfg.socket && *cfg.socket) {
    unlink(cfg.socket);
  }
  loop_free();
  close(sig_fd);
  model_free();
  store_free();
  outputs_free();
  config_free(&cfg);
  return 0;
}
