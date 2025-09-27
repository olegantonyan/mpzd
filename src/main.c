#include "mediaplayer/mediaplayer.h"
#include "rpc/server.h"

#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>

struct termios orig_termios;

void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    atexit(disable_raw_mode);
}

void cb(int fd, void *buffer, int buffer_size, int recv_size, int *send_bytes) {
  printf("received: %s from %d\n", buffer, fd);

  //memset(buffer, sizeof(buffer), 0);
  snprintf(buffer, buffer_size, "response to you %d", fd);
  *send_bytes = strlen(buffer);
}

int main(int argc, char **argv) {
  ServerHandle_t h;
  ServerResult_t r = server_init(&h, 6600, cb);
  if (!r.ok) {
    puts(r.error_string);
    return 1;
  }

  while (1) {
    h.poll_func(&h);
  }




    if (argc < 2) {
        fprintf(stderr, "Usage: %s <audio-file>\n", argv[0]);
        return 1;
    }

    MediaPlayerHandle_t handle;
    MediaPlayerResult_t res = mediaplayer_init(&handle);
    if (!res.ok) {
      fprintf(stderr, "Error: %s\n", res.error_string);
      return 1;
    }

    res = mediaplayer_play(&handle, argv[1]);
    if (!res.ok) {
      fprintf(stderr, "Error: %s\n", res.error_string);
      return 1;
    }

    printf("Playing audio: %s\n", argv[1]);
    printf("Press SPACE to pause/resume, q to quit.\n");

    enable_raw_mode();

    bool paused = false;
    bool quit = false;
    char c;

    while (!quit) {
        char c;
        if (read(STDIN_FILENO, &c, 1) == 1) {
            if (c == ' ') {
                if (paused) {
                  mediaplayer_unpause(&handle);
                } else {
                  mediaplayer_pause(&handle);
                }
                paused = !paused;
            } else if (c == 'q') {
                quit = true;
            }
        }
        usleep(5000);
    }

    mediaplayer_deinit(&handle);
    return 0;
}
