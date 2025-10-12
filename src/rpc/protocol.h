#ifndef RPC_PROTOCOL_H
#define RPC_PROTOCOL_H

#include <stdbool.h>
#include <unistd.h>

typedef enum {
  PROTOCOL_COMMAND_TYPE_INTEGER,
  PROTOCOL_COMMAND_TYPE_STRING
} ProtocolCommandArgumentType_t;

typedef union {
  int valueint;
  char *valuestring;
} ProtocolCommandArgumentValue_t;

typedef struct {
  ProtocolCommandArgumentType_t type;
  ProtocolCommandArgumentValue_t value;
} ProtocolCommandArgument_t;

typedef bool (*ProtocolCommandFunction_t)(const ProtocolCommandArgument_t *args, int args_count, ProtocolCommandArgument_t *results, int results_count);

bool protocol_register(const char *command, ProtocolCommandFunction_t handler);
bool protocol_handle(void *buffer, ssize_t buffer_size, ssize_t input_size, ssize_t *output_size);

#endif
