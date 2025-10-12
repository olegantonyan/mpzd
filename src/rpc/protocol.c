#include "rpc/protocol.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_COMMANDS 100

typedef struct {
  const char *cmd;
  ProtocolCommandFunction_t func;
} ProtocolCommand_t;

static ProtocolCommand_t registered_commands[MAX_COMMANDS] = { 0 };
static size_t registered_commands_count = 0;

bool protocol_register(const char *command, ProtocolCommandFunction_t handler) {
  if (registered_commands_count >= MAX_COMMANDS) {
    return false;
  }

  ProtocolCommand_t c = { .cmd = command, .func = handler };
  registered_commands[registered_commands_count] = c;
  registered_commands_count++;
  return true;
}

bool protocol_handle(void *buffer, ssize_t buffer_size, ssize_t input_size, ssize_t *output_size) {
  *output_size = 0;

  cJSON *json = cJSON_Parse(buffer);
  if (json == NULL) {
    const char *error_ptr = cJSON_GetErrorPtr();
    if (error_ptr != NULL) {
      fprintf(stderr, "json parse error: %s\n", error_ptr);
    }
    cJSON_Delete(json);
    return false;
  }

  cJSON *cmd = cJSON_GetObjectItemCaseSensitive(json, "cmd");
  if (!cJSON_IsString(cmd) || (cmd->valuestring == NULL)) {
    fprintf(stderr, "no command field, skipping\n");
    cJSON_Delete(json);
    return false;
  }

  const char *command = cmd->valuestring;

  for (int i = 0; i < registered_commands_count; i++) {
    ProtocolCommand_t *c = &registered_commands[i];
    if (strcmp(c->cmd, command) == 0) {
      cJSON *result = cJSON_CreateObject();
      if (!c->func(json, result)) {
        fprintf(stderr, "error executing command '%s'\n", command);
        cJSON_Delete(result);
        cJSON_Delete(json);
        return false;
      } else {
        if (!cJSON_PrintPreallocated(result, buffer, buffer_size - 5, 0)) {
          fprintf(stderr, "error printing command results\n");
          cJSON_Delete(result);
          cJSON_Delete(json);
          return false;
        }
      }
      cJSON_Delete(result);
      break;
    }
  }

  cJSON_Delete(json);
  *output_size = strlen(buffer);
  return true;
}
