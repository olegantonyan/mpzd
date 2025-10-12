#include "commands/commands.h"
#include "rpc/protocol.h"
#include "version.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <cjson/cJSON.h>

static bool info(const cJSON *input, cJSON *output) {
  cJSON_AddStringToObject(output, "version", PROJECT_VERSION);

  return true;
}

void commands_init() {
  protocol_register("info", info);
}
