#ifndef UTIL_WAKE_H
#define UTIL_WAKE_H

int wake_open(void);
void wake_signal(int fd);
void wake_drain(int fd);
void wake_close(int fd);

#endif
