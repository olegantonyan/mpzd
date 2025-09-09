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

MediaPlayerResult_t mediaplayer_load(MediaPlayerHandle_t *handle, const char *filepath) {
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

MediaPlayerResult_t mediaplayer_deinit(MediaPlayerHandle_t *handle) {
  MediaPlayerResult_t result = { .ok = true, .error_string = NULL };

  mpv_terminate_destroy(handle->h);
  handle->h = NULL;

  return result;
}
