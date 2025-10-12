#ifndef RPC_PROTOCOL_H
#define RPC_PROTOCOL_H

#include <stdbool.h>
#include <unistd.h>
#include <cjson/cJSON.h>

typedef bool (*ProtocolCommandFunction_t)(const cJSON *arguments, cJSON *result);

bool protocol_register(const char *command, ProtocolCommandFunction_t handler);
bool protocol_handle(void *buffer, ssize_t buffer_size, ssize_t input_size, ssize_t *output_size);

#endif
