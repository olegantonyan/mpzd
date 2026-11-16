#ifndef UTIL_LOOP_H
#define UTIL_LOOP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct LoopFd LoopFd;
typedef struct LoopTimer LoopTimer;
typedef void (*LoopFdCb)(void *ud, short revents);
typedef void (*LoopTimerCb)(void *ud);

LoopFd *loop_add_fd(int fd, short events, LoopFdCb cb, void *ud);
void loop_mod(LoopFd *h, short events);
void loop_del(LoopFd *h);

LoopTimer *loop_timer_new(LoopTimerCb cb, void *ud);
void loop_timer_arm(LoopTimer *t, uint64_t deadline_ms);
void loop_timer_free(LoopTimer *t);

void loop_set_hooks(void (*after_cb)(void), void (*after_batch)(void));
bool loop_run_once(int timeout_ms);
void loop_run(void);
void loop_stop(void);
void loop_free(void);

#endif
