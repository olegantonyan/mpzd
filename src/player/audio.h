#ifndef PLAYER_AUDIO_H
#define PLAYER_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint64_t token;
  const char *path;
  uint32_t begin_ms;
  uint32_t end_ms;
  bool stream;
} AudioItem;

typedef enum { AEV_STARTED, AEV_NEED_NEXT, AEV_STOPPED, AEV_ERROR, AEV_ICY } AudioEventType;

typedef struct {
  AudioEventType type;
  uint64_t token;
  char text[256];
} AudioEvent;

bool audio_init(const char *device, int *event_fd);
void audio_shutdown(void);
void audio_play(const AudioItem *item, uint32_t start_ms, bool paused);
void audio_next(const AudioItem *item, uint64_t for_token);
void audio_pause(bool pause);
void audio_stop(void);
void audio_set_volume(int volume);
bool audio_poll_event(AudioEvent *ev);
void audio_ack_events(void);
uint32_t audio_position_ms(void);

#endif
