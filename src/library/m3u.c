#include "library/m3u.h"
#include "util/util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define M3U_MAX_LINE 8192

static char *trim(char *s) {
  while (isspace((unsigned char)*s)) s++;
  char *e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1])) e--;
  *e = '\0';
  return s;
}

static void add_entry(const char *playlist, const char *entry, StrVec *entries) {
  if (!*entry) {
    return;
  }
  if (strncasecmp(entry, "file://", 7) == 0) {
    entry += 7;
  }
  if (path_is_url(entry) || entry[0] == '/') {
    strvec_push(entries, xstrdup(entry));
    return;
  }
  StrBuf p = { 0 };
  sb_append(&p, playlist, path_dirname_len(playlist));
  sb_puts(&p, "/");
  sb_puts(&p, entry);
  strvec_push(entries, p.data);
}

bool m3u_parse(const char *path, StrVec *entries) {
  FILE *f = fopen(path, "r");
  if (!f) {
    return false;
  }
  bool pls = strcasecmp(path_ext(path), "pls") == 0;
  char *line = xmalloc(M3U_MAX_LINE);
  bool first = true;
  while (fgets(line, M3U_MAX_LINE, f)) {
    char *s = line;
    if (first && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) {
      s += 3;
    }
    first = false;
    s = trim(s);
    if (pls) {
      if (strncasecmp(s, "File", 4) == 0 && isdigit((unsigned char)s[4])) {
        char *eq = strchr(s, '=');
        if (eq) {
          add_entry(path, trim(eq + 1), entries);
        }
      }
    } else if (*s != '#') {
      add_entry(path, s, entries);
    }
  }
  free(line);
  fclose(f);
  return true;
}
