#ifndef UTIL_BG_H
#define UTIL_BG_H

#include <stdbool.h>

typedef void (*BgWork)(void *arg);
typedef void (*BgDone)(void *arg, bool timed_out);
typedef void (*BgFree)(void *arg);

bool bg_init(int *event_fd, int *timer_fd);
bool bg_run(void *arg, BgWork work, BgDone done, BgFree free_fn, unsigned timeout_ms);
void bg_on_event(void *ud);
void bg_on_timer(void *ud);
void bg_shutdown(void);

#endif
