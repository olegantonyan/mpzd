#include "mediaplayer/mediaplayer.h"

#include <mpv/client.h>
#include <stdlib.h>
#include <stdio.h>

MediaPlayerResult_t mediaplayer_init(MediaPlayerHandle_t *handle) {
  MediaPlayerResult_t result = { .ok = true, .error_string = NULL };
  int error = 0;

  if (setenv("LC_NUMERIC", "C", 1) != 0) {
    result.ok = false;
    result.error_string = "setenv failed";
    return result;
  }

  handle->h = mpv_create();
  if (!handle->h) {
    result.ok = false;
    result.error_string = "mpv create failed";
    return result;
  }

  error = mpv_set_option_string(handle->h, "vo", "null");
  if (error < 0) {
    result.ok = false;
    result.error_string = mpv_error_string(error);
    return result;
  }
  error = mpv_set_option_string(handle->h, "input-default-bindings", "no");
  if (error < 0) {
    result.ok = false;
    result.error_string = mpv_error_string(error);
    return result;
  }
  error = mpv_set_option_string(handle->h, "input-vo-keyboard", "no");
  if (error < 0) {
    result.ok = false;
    result.error_string = mpv_error_string(error);
    return result;
  }

  error = mpv_initialize(handle->h);
  if (error < 0) {
    result.ok = false;
    result.error_string = mpv_error_string(error);
    return result;
  }

  return result;
}

MediaPlayerResult_t mediaplayer_deinit(MediaPlayerHandle_t *handle) {
  MediaPlayerResult_t result = { .ok = true, .error_string = NULL };

  mpv_terminate_destroy(handle->h);
  handle->h = NULL;

  return result;
}

MediaPlayerResult_t mediaplayer_play(MediaPlayerHandle_t *handle, const char *filepath) {
  MediaPlayerResult_t result = { .ok = true, .error_string = NULL };

  const char *loadfile[] = {"loadfile", filepath, NULL};
  int error = mpv_command(handle->h, loadfile);
  if (error < 0) {
    result.ok = false;
    result.error_string = mpv_error_string(error);
  }

  return result;
}

MediaPlayerResult_t mediaplayer_pause(MediaPlayerHandle_t *handle) {
  MediaPlayerResult_t result = { .ok = true, .error_string = NULL };

  int paused_int = 1;
  int error = mpv_set_property(handle->h, "pause", MPV_FORMAT_FLAG, &paused_int);
  if (error < 0) {
    result.ok = false;
    result.error_string = mpv_error_string(error);
  }

  return result;
}

MediaPlayerResult_t mediaplayer_unpause(MediaPlayerHandle_t *handle) {
  MediaPlayerResult_t result = { .ok = true, .error_string = NULL };

  int paused_int = 0;
  int error = mpv_set_property(handle->h, "pause", MPV_FORMAT_FLAG, &paused_int);
  if (error < 0) {
    result.ok = false;
    result.error_string = mpv_error_string(error);
  }

  return result;
}

MediaPlayerResult_t mediaplayer_stop(MediaPlayerHandle_t *handle) {
  MediaPlayerResult_t result = { .ok = true, .error_string = NULL };

  const char *stop_cmd[] = {"stop", NULL};
  int error = mpv_command(handle->h, stop_cmd);
  if (error < 0) {
    result.ok = false;
    result.error_string = mpv_error_string(error);
  }
  return result;
}

MediaPlayerResult_t mediaplayer_get_state(MediaPlayerHandle_t *handle, MediaPlayerState_t *out_value) {
  MediaPlayerResult_t result = { .ok = true, .error_string = NULL };
  int error = 0;

  int paused = 0;
  error = mpv_get_property(handle->h, "pause", MPV_FORMAT_FLAG, &paused);
  if (error < 0) {
    result.ok = false;
    result.error_string = mpv_error_string(error);
    return result;
  }
  if (paused) {
    *out_value = MEDIAPLAYER_PAUSED;
    return result;
  }

  int eof = 0;
  error = mpv_get_property(handle->h, "eof-reached", MPV_FORMAT_FLAG, &eof);
  if (error < 0) {
    result.ok = false;
    result.error_string = mpv_error_string(error);
    return result;
  }
  if (eof) {
    *out_value = MEDIAPLAYER_STOPPED;
    return result;
  }

  *out_value = MEDIAPLAYER_PLAYING;

  return result;
}

MediaPlayerResult_t mediaplayer_get_position(MediaPlayerHandle_t *handle, double *out_value) {
  MediaPlayerResult_t result = { .ok = true, .error_string = NULL };

  int error = mpv_get_property(handle->h, "time-pos", MPV_FORMAT_DOUBLE, out_value);
  if (error < 0) {
    result.ok = false;
    result.error_string = mpv_error_string(error);
  }

  return result;
}

MediaPlayerResult_t mediaplayer_get_duration(MediaPlayerHandle_t *handle, double *out_value) {
  MediaPlayerResult_t result = { .ok = true, .error_string = NULL };

  int error = mpv_get_property(handle->h, "duration", MPV_FORMAT_DOUBLE, out_value);
  if (error < 0) {
    result.ok = false;
    result.error_string = mpv_error_string(error);
  }

  return result;
}

MediaPlayerResult_t mediaplayer_set_position(MediaPlayerHandle_t *handle, double value) {
  MediaPlayerResult_t result = { .ok = true, .error_string = NULL };

  int error = mpv_set_property(handle->h, "time-pos", MPV_FORMAT_DOUBLE, &value);
  if (error < 0) {
    result.ok = false;
    result.error_string = mpv_error_string(error);
  }

  return result;
}
