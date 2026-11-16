#ifndef PROTO_PROTO_H
#define PROTO_PROTO_H

#include "net/server.h"

#include <stdbool.h>
#include <stdint.h>

#define PROTO_VERSION 1
#define PROTO_MAX_ARGS 1024

enum { E_SYNTAX = 1, E_ARG = 2, E_NOT_FOUND = 3, E_IO = 4, E_STATE = 5 };

typedef bool (*CmdFn)(int argc, char **argv);
typedef struct Deferred Deferred;

typedef struct {
  const char *name;
  CmdFn fn;
  int min_args;
  int max_args;
} Command;

extern const Command cmd_meta[];
extern const Command cmd_library[];
extern const Command cmd_playlist[];
extern const Command cmd_player[];

int proto_tokenize(char *line, char **argv, int max);
void proto_handle_line(Client *c, char *line);
const char *proto_greeting(void);
void proto_list_commands(void);

Client *req_client(void);
void r_kv(const char *key, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void r_str(const char *key, const char *value);
void r_opt(const char *key, const char *value);
void r_u32(const char *key, uint32_t value);
void r_opt_u32(const char *key, uint32_t value);
bool r_err(int code, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

bool arg_u32(const char *s, uint32_t *out, const char *what);

#define FS_TIMEOUT_MS 5000

Deferred *r_defer(void);
bool r_async(void *job, Deferred **slot, void (*work)(void *), void (*done)(void *, bool), void (*free_fn)(void *), unsigned timeout_ms);
bool r_resume(Deferred *d, bool timed_out);
void r_finish(Deferred *d, bool ok);

#endif
