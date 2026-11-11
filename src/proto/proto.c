#include "proto/proto.h"
#include "util/util.h"

#include <stdarg.h>
#include <string.h>

static const Command *const tables[] = { cmd_meta, cmd_library, cmd_playlist, cmd_player };

static struct {
  Client *client;
  StrBuf reply;
  bool failed;
} R;

int proto_tokenize(char *s, char **argv, int max) {
  int argc = 0;
  for (;;) {
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) {
      return argc;
    }
    if (argc == max) {
      return -1;
    }
    if (*s == '"') {
      char *out = ++s;
      argv[argc++] = out;
      while (*s && *s != '"') {
        if (*s == '\\' && s[1]) {
          s++;
        }
        *out++ = *s++;
      }
      if (*s != '"') {
        return -1;
      }
      s++;
      if (*s && *s != ' ' && *s != '\t') {
        return -1;
      }
      *out = '\0';
    } else {
      argv[argc++] = s;
      while (*s && *s != ' ' && *s != '\t') {
        if (*s == '"') {
          return -1;
        }
        s++;
      }
      if (*s) {
        *s++ = '\0';
      }
    }
  }
}

const char *proto_greeting(void) {
  return "mpzd 1\n";
}

static const Command *find_command(const char *name) {
  for (size_t t = 0; t < ARRAY_LEN(tables); t++) {
    for (const Command *c = tables[t]; c->name; c++) {
      if (strcmp(c->name, name) == 0) {
        return c;
      }
    }
  }
  return NULL;
}

void proto_list_commands(void) {
  for (size_t t = 0; t < ARRAY_LEN(tables); t++) {
    for (const Command *c = tables[t]; c->name; c++) {
      r_str("command", c->name);
    }
  }
}

Client *req_client(void) {
  return R.client;
}

static void append_sanitized(const char *s) {
  const char *run = s;
  for (; *s; s++) {
    if (*s == '\n' || *s == '\r') {
      sb_append(&R.reply, run, (size_t)(s - run));
      sb_puts(&R.reply, " ");
      run = s + 1;
    }
  }
  sb_append(&R.reply, run, (size_t)(s - run));
}

void r_str(const char *key, const char *value) {
  sb_puts(&R.reply, key);
  sb_puts(&R.reply, ": ");
  append_sanitized(value);
  sb_puts(&R.reply, "\n");
}

void r_opt(const char *key, const char *value) {
  if (value && *value) {
    r_str(key, value);
  }
}

void r_u32(const char *key, uint32_t value) {
  sb_printf(&R.reply, "%s: %u\n", key, value);
}

void r_opt_u32(const char *key, uint32_t value) {
  if (value) {
    r_u32(key, value);
  }
}

void r_kv(const char *key, const char *fmt, ...) {
  StrBuf v = { 0 };
  va_list ap;
  va_start(ap, fmt);
  sb_vprintf(&v, fmt, ap);
  va_end(ap);
  r_str(key, v.data ? v.data : "");
  sb_free(&v);
}

bool r_err(int code, const char *fmt, ...) {
  if (R.failed) {
    return false;
  }
  R.failed = true;
  sb_reset(&R.reply);
  sb_printf(&R.reply, "err %d ", code);
  StrBuf msg = { 0 };
  va_list ap;
  va_start(ap, fmt);
  sb_vprintf(&msg, fmt, ap);
  va_end(ap);
  append_sanitized(msg.data ? msg.data : "");
  sb_free(&msg);
  sb_puts(&R.reply, "\n");
  return false;
}

bool arg_u32(const char *s, uint32_t *out, const char *what) {
  if (!parse_u32(s, out)) {
    return r_err(E_ARG, "invalid %s '%s'", what, s);
  }
  return true;
}

void proto_handle_line(Client *c, char *line) {
  char *argv[PROTO_MAX_ARGS];
  R.client = c;
  R.failed = false;
  sb_reset(&R.reply);

  int argc = proto_tokenize(line, argv, PROTO_MAX_ARGS);
  if (argc < 0) {
    r_err(E_SYNTAX, "syntax error");
  } else if (argc > 0) {
    const Command *cmd = find_command(argv[0]);
    int nargs = argc - 1;
    if (!cmd) {
      r_err(E_SYNTAX, "unknown command '%s'", argv[0]);
    } else if (nargs < cmd->min_args || (cmd->max_args >= 0 && nargs > cmd->max_args)) {
      r_err(E_SYNTAX, "wrong number of arguments for '%s'", cmd->name);
    } else if (cmd->fn(nargs, argv + 1) && !R.failed) {
      sb_puts(&R.reply, "ok\n");
    } else if (!R.failed) {
      r_err(E_IO, "command failed");
    }
  } else {
    return;
  }
  client_send(c, R.reply.data, R.reply.len);
  if (R.reply.cap > 64 * 1024) {
    sb_free(&R.reply);
  }
  R.client = NULL;
}
