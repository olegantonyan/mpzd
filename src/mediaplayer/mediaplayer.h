#ifndef MEDIAPLAYER_MEDIAPLAYER_H
#define MEDIAPLAYER_MEDIAPLAYER_H

#include <stdbool.h>

typedef struct {
  bool ok;
  const char *error_string;
} MediaPlayerResult_t;

typedef struct {
  void *h;
} MediaPlayerHandle_t;

MediaPlayerResult_t mediaplayer_init(MediaPlayerHandle_t *handle);
MediaPlayerResult_t mediaplayer_deinit(MediaPlayerHandle_t *handle);
MediaPlayerResult_t mediaplayer_load(MediaPlayerHandle_t *handle, const char *filepath);
MediaPlayerResult_t mediaplayer_pause(MediaPlayerHandle_t *handle);
MediaPlayerResult_t mediaplayer_unpause(MediaPlayerHandle_t *handle);

#endif
