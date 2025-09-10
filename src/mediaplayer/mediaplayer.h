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

typedef enum {
  MEDIAPLAYER_STOPPED,
  MEDIAPLAYER_PLAYING,
  MEDIAPLAYER_PAUSED
} MediaPlayerState_t;

MediaPlayerResult_t mediaplayer_init(MediaPlayerHandle_t *handle);
MediaPlayerResult_t mediaplayer_deinit(MediaPlayerHandle_t *handle);
MediaPlayerResult_t mediaplayer_play(MediaPlayerHandle_t *handle, const char *filepath);
MediaPlayerResult_t mediaplayer_pause(MediaPlayerHandle_t *handle);
MediaPlayerResult_t mediaplayer_unpause(MediaPlayerHandle_t *handle);
MediaPlayerResult_t mediaplayer_stop(MediaPlayerHandle_t *handle);
MediaPlayerResult_t mediaplayer_get_state(MediaPlayerHandle_t *handle, MediaPlayerState_t *out_value);
MediaPlayerResult_t mediaplayer_get_position(MediaPlayerHandle_t *handle, double *out_value);
MediaPlayerResult_t mediaplayer_get_duration(MediaPlayerHandle_t *handle, double *out_value);
MediaPlayerResult_t mediaplayer_set_position(MediaPlayerHandle_t *handle, double value);

#endif
