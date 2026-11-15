#ifndef PLAYER_PLAYER_H
#define PLAYER_PLAYER_H

#include "store/store.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum { PS_STOPPED, PS_PLAYING, PS_PAUSED } PlayState;

typedef struct {
  uint32_t pl;
  uint32_t track;
} TrackRef;

typedef struct {
  PlayState state;
  bool has_track;
  TrackRef cur;
  uint32_t pos_ms;
  const char *format;
  const char *icy;
} PlayerStatus;

void player_init(const StorePlaying *resume);
void player_on_audio(void *ud);
void player_status(PlayerStatus *st);
StorePlaying player_store_state(void);

bool player_play(TrackRef ref);
bool player_play_default(void);
bool player_pause(void);
bool player_resume(void);
void player_stop(void);
bool player_next(void);
bool player_prev(void);
bool player_seek(int64_t ms, bool relative);
void player_set_volume(int volume);
void player_set_output(const char *id);
void player_select(TrackRef ref);
void player_sync_hint(uint32_t pl_id);
void player_playlist_deleted(uint32_t pl_id);

const char *play_state_name(PlayState s);

#endif
