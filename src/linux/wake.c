#include "util/wake.h"
#include "util/util.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#define LOG_MODULE "wake"

int wake_open(void) {
  return eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
}

void wake_signal(int fd) {
  uint64_t one = 1;
  if (write(fd, &one, sizeof(one)) < 0) {
    LOG("eventfd write: %s", strerror(errno));
  }
}

void wake_drain(int fd) {
  uint64_t v;
  if (read(fd, &v, sizeof(v)) < 0 && errno != EAGAIN) {
    LOG("eventfd read: %s", strerror(errno));
  }
}

void wake_close(int fd) {
  if (fd >= 0) {
    close(fd);
  }
}
