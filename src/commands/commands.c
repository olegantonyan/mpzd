#include "commands/commands.h"
#include "rpc/protocol.h"
#include "version.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <cjson/cJSON.h>

#define LOG_MODULE "[commands] "

static const CommandsContext_t *context = NULL;

static bool info(const cJSON *input, cJSON *output) {
  cJSON_AddStringToObject(output, "version", PROJECT_VERSION);

  return true;
}

static bool play(const cJSON *input, cJSON *output) {
  cJSON *filepath = cJSON_GetObjectItemCaseSensitive(input, "filepath");
  if (!cJSON_IsString(filepath) || (filepath->valuestring == NULL)) {
    cJSON_AddStringToObject(output, "error", "no filepath provided");
    return false;
  }

  MediaPlayerResult_t res = mediaplayer_play(context->mp, filepath->valuestring);
  if (!res.ok) {
    cJSON_AddStringToObject(output, "error", res.error_string);
    return false;
  }

  return true;
}

static bool stop(const cJSON *input, cJSON *output) {
  MediaPlayerResult_t res = mediaplayer_stop(context->mp);
  if (!res.ok) {
    cJSON_AddStringToObject(output, "error", res.error_string);
    return false;
  }

  return true;
}

static bool _pause(const cJSON *input, cJSON *output) {
  MediaPlayerResult_t res = mediaplayer_pause(context->mp);
  if (!res.ok) {
    cJSON_AddStringToObject(output, "error", res.error_string);
    return false;
  }

  return true;
}

static bool unpause(const cJSON *input, cJSON *output) {
  MediaPlayerResult_t res = mediaplayer_unpause(context->mp);
  if (!res.ok) {
    cJSON_AddStringToObject(output, "error", res.error_string);
    return false;
  }

  return true;
}

static bool get_state(const cJSON *input, cJSON *output) {
  MediaPlayerState_t state;
  MediaPlayerResult_t res = mediaplayer_get_state(context->mp, &state);
  if (!res.ok) {
    if (strcmp(res.error_string, "property unavailable") == 0) {
      state = MEDIAPLAYER_STOPPED;
    } else {
      cJSON_AddStringToObject(output, "error", res.error_string);
      return false;
    }
  }

  switch(state) {
    case MEDIAPLAYER_STOPPED:
      cJSON_AddStringToObject(output, "state", "stopped");
      break;
    case MEDIAPLAYER_PLAYING:
      cJSON_AddStringToObject(output, "state", "playing");
      break;
    case MEDIAPLAYER_PAUSED:
      cJSON_AddStringToObject(output, "state", "paused");
      break;
    default:
      cJSON_AddStringToObject(output, "error", "unknown state");
  }

  return true;
}

static bool ls(const cJSON *input, cJSON *output) {
  cJSON *path = cJSON_GetObjectItemCaseSensitive(input, "path");
  if (!cJSON_IsString(path) || (path->valuestring == NULL)) {
    cJSON_AddStringToObject(output, "error", "no path provided");
    return false;
  }

  DIR *dir = opendir(path->valuestring);
  if (!dir) {
    cJSON_AddStringToObject(output, "error", strerror(errno));
    return false;
  }

  cJSON *files = cJSON_CreateArray();
  cJSON *dirs = cJSON_CreateArray();

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    char fullpath[10000];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", path->valuestring, entry->d_name);

    struct stat st;
    if (stat(fullpath, &st) == -1) {
      perror(LOG_MODULE"ls: stat");
      continue;
    }

    if (S_ISDIR(st.st_mode)) {
      cJSON_AddItemToArray(dirs, cJSON_CreateString(entry->d_name));
    } else if (S_ISREG(st.st_mode)) {
      cJSON_AddItemToArray(files, cJSON_CreateString(entry->d_name));
    }
  }

  cJSON_AddItemToObject(output, "files", files);
  cJSON_AddItemToObject(output, "dirs", dirs);

  closedir(dir);

  return true;
}

void commands_init(const CommandsContext_t *ctx) {
  context = ctx;
  protocol_register("info", info);
  protocol_register("play", play);
  protocol_register("stop", stop);
  protocol_register("pause", _pause);
  protocol_register("unpause", unpause);
  protocol_register("get_state", get_state);
  protocol_register("ls", ls);
}
