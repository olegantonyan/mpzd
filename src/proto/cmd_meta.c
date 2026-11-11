#include "proto/proto.h"
#include "version.h"

#include <string.h>

static bool ping(int argc, char **argv) {
  return true;
}

static bool version(int argc, char **argv) {
  r_str("version", PROJECT_VERSION);
  r_u32("protocol", PROTO_VERSION);
  return true;
}

static bool commands(int argc, char **argv) {
  proto_list_commands();
  return true;
}

static bool watch(int argc, char **argv) {
  if (strcmp(argv[0], "on") == 0) {
    client_set_watch(req_client(), true);
  } else if (strcmp(argv[0], "off") == 0) {
    client_set_watch(req_client(), false);
  } else {
    return r_err(E_ARG, "expected on|off");
  }
  return true;
}

static bool close_cmd(int argc, char **argv) {
  client_close_later(req_client());
  return true;
}

const Command cmd_meta[] = {
  { "ping", ping, 0, 0 },
  { "version", version, 0, 0 },
  { "commands", commands, 0, 0 },
  { "watch", watch, 1, 1 },
  { "close", close_cmd, 0, 0 },
  { NULL, NULL, 0, 0 },
};
