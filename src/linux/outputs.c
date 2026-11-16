#include "linux/outputs_linux.h"
#include "linux/pw_sinks.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define PW_DEFAULT "default"

static struct {
  bool pipewire;
  OutputList fixed;
} O;

static ssize_t find(const OutputList *l, const char *id) {
  for (size_t i = 0; i < l->len; i++) {
    if (strcmp(l->v[i].id, id) == 0) {
      return (ssize_t)i;
    }
  }
  return -1;
}

bool outputs_init(bool pipewire, const StrVec *specs, const char *device) {
#ifndef MPZD_PIPEWIRE
  if (pipewire) {
    return false;
  }
#endif
  O.pipewire = pipewire;
  if (pipewire) {
    pw_sinks_init();
    return true;
  }
  for (size_t i = 0; i < specs->len; i++) {
    const char *s = specs->v[i];
    size_t n = 0;
    while (s[n] && !isspace((unsigned char)s[n])) n++;
    char *id = xstrndup(s, n);
    const char *desc = s + n;
    while (isspace((unsigned char)*desc)) desc++;
    if (*id && find(&O.fixed, id) < 0) {
      output_list_push(&O.fixed, id, desc);
    }
    free(id);
  }
  if (find(&O.fixed, device) < 0) {
    output_list_push(&O.fixed, device, NULL);
  }
  return true;
}

void outputs_free(void) {
  if (O.pipewire) {
    pw_sinks_deinit();
  }
  output_list_free(&O.fixed);
}

bool outputs_list(OutputList *out) {
  if (!O.pipewire) {
    for (size_t i = 0; i < O.fixed.len; i++) {
      output_list_push(out, O.fixed.v[i].id, O.fixed.v[i].desc);
    }
    return true;
  }
  output_list_push(out, PW_DEFAULT, "Default");
  return pw_sinks_list(out);
}

bool outputs_valid(const char *id) {
  if (!O.pipewire) {
    return find(&O.fixed, id) >= 0;
  }
  if (strcmp(id, PW_DEFAULT) == 0) {
    return true;
  }
  OutputList l = { 0 };
  bool ok = outputs_list(&l) && find(&l, id) >= 0;
  output_list_free(&l);
  return ok;
}

char *outputs_pcm(const char *id) {
  if (!O.pipewire) {
    return xstrdup(id);
  }
  if (strcmp(id, PW_DEFAULT) == 0) {
    return xstrdup("pipewire");
  }
  StrBuf sb = { 0 };
  sb_puts(&sb, "pipewire:NODE=\"");
  for (const char *p = id; *p; p++) {
    if (*p == '"' || *p == '\\') {
      sb_append(&sb, "\\", 1);
    }
    sb_append(&sb, p, 1);
  }
  sb_puts(&sb, "\"");
  return sb.data;
}
