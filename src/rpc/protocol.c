#include "rpc/protocol.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <cjson/cJSON.h>

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
    cJSON_Delete(json);
    return false;
  }

  const char *command = cmd->valuestring;
  ProtocolCommandArgument_t *arguments = NULL;
  int arguemnts_count = 0;

  cJSON *args = cJSON_GetObjectItemCaseSensitive(json, "args");
  if (cJSON_IsArray(args)) {
    arguemnts_count = cJSON_GetArraySize(args);
    arguments = malloc(arguemnts_count * sizeof(ProtocolCommandArgument_t));
    for (int i = 0; i < arguemnts_count; i++) {
      cJSON *item = cJSON_GetArrayItem(args, i);
      if (cJSON_IsString(item)) {
        ProtocolCommandArgumentValue_t v = { .valuestring = item->valuestring };
        ProtocolCommandArgument_t a = { .type = PROTOCOL_COMMAND_TYPE_STRING, .value = v };
        arguments[i] = a;
      } else if (cJSON_IsNumber(item)) {
        ProtocolCommandArgumentValue_t v = { .valueint = item->valueint };
        ProtocolCommandArgument_t a = { .type = PROTOCOL_COMMAND_TYPE_INTEGER, .value = v };
        arguments[i] = a;
      }
    }
  }

  for (int i = 0; i < registered_commands_count; i++) {
    ProtocolCommand_t *c = &registered_commands[i];
    if (strcmp(c->cmd, command) == 0) {
    //  c->func();
      break;
    }
  }

  snprintf(buffer, buffer_size, "your command was: %s %d\n", cmd->valuestring, arguments[0].value.valueint);
  *output_size = strlen(buffer);

  cJSON_Delete(json);
  if (arguments) {
    free(arguments);
  }
  return true;
}
