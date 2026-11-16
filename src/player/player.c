#include "player/player.h"
#include "app.h"
#include "model/model.h"
#include "net/server.h"
#include "player/audio.h"
#include "player/outputs.h"
#include "util/util.h"

#include <stdlib.h>
#include <string.h>

#define LOG_MODULE "player"
#define TRAIL_MAX 64
#define SENT_MAX 8
#define RECENT_AVOID 16
#define RANDOM_TRIES 64
#define MAX_CONSECUTIVE_ERRORS 50
#define START_TIMEOUT_MS 20000

typedef enum { NAV_NEW, NAV_BACK, NAV_FWD, NAV_SEEK } Nav;

typedef struct {
  uint64_t token;
  TrackRef ref;
  size_t idx;
  Nav nav;
} Sent;

static struct {
  PlayState state;
  bool has_cur;
  TrackRef cur;
  size_t cur_idx;
  uint64_t started_token;
  uint64_t pending_token;
  uint64_t pending_since;
  uint32_t pending_pos;
  bool has_resume;
  uint32_t resume_ms;
  char format[256];
  char icy[256];
  Sent sent[SENT_MAX];
  unsigned sent_n;
  TrackRef trail[TRAIL_MAX];
  int trail_len;
  int trail_pos;
  uint64_t sel_stamp;
  uint64_t decided_stamp;
  uint32_t errors;
  uint64_t next_token;
} P;

static const char *const state_names[] = { "stop", "play", "pause" };

const char *play_state_name(PlayState s) {
  return state_names[s];
}

static bool ref_eq(TrackRef a, TrackRef b) {
  return a.pl == b.pl && a.track == b.track;
}

static bool ref_valid(TrackRef r) {
  Playlist *pl = model_find(r.pl);
  return pl && playlist_index(pl, r.track) >= 0;
}

static void changed(void) {
  M.dirty = true;
  server_event("player");
  app_changed();
}

static Sent *lookup(uint64_t token) {
  for (unsigned i = 0; i < SENT_MAX; i++) {
    if (P.sent[i].token == token && token) {
      return &P.sent[i];
    }
  }
  return NULL;
}

static bool make_item(TrackRef r, Nav nav, AudioItem *it) {
  Playlist *pl = model_find(r.pl);
  if (!pl) {
    return false;
  }
  ssize_t idx = playlist_index(pl, r.track);
  if (idx < 0) {
    return false;
  }
  const Track *t = &pl->tracks[idx];
  it->token = ++P.next_token;
  it->path = track_str(t, TS_PATH);
  it->begin_ms = t->begin_ms;
  it->end_ms = t->end_ms;
  it->stream = (t->flags & TRACK_STREAM) != 0;
  P.sent[P.sent_n++ % SENT_MAX] = (Sent){ .token = it->token, .ref = r, .idx = (size_t)idx, .nav = nav };
  return true;
}

static bool start(TrackRef r, uint32_t start_ms, Nav nav, bool paused) {
  AudioItem it;
  if (!make_item(r, nav, &it)) {
    return false;
  }
  audio_play(&it, start_ms, paused);
  P.pending_token = it.token;
  P.pending_since = mono_ms();
  P.pending_pos = start_ms;
  if (nav != NAV_SEEK) {
    P.decided_stamp = P.sel_stamp;
  }
  P.state = paused ? PS_PAUSED : PS_PLAYING;
  P.has_resume = false;
  P.cur = r;
  P.has_cur = true;
  P.cur_idx = P.sent[(P.sent_n - 1) % SENT_MAX].idx;
  changed();
  return true;
}

static void trail_record(TrackRef r, Nav nav) {
  if (nav == NAV_BACK && P.trail_pos > 0) {
    P.trail_pos--;
    return;
  }
  if (nav == NAV_FWD && P.trail_pos < P.trail_len - 1) {
    P.trail_pos++;
    return;
  }
  P.trail_len = P.trail_pos + 1;
  if (P.trail_len == TRAIL_MAX) {
    memmove(P.trail, P.trail + 1, sizeof(TrackRef) * (TRAIL_MAX - 1));
    P.trail_len--;
  }
  P.trail[P.trail_len++] = r;
  P.trail_pos = P.trail_len - 1;
}

static bool recently_played(TrackRef r, int window) {
  for (int i = P.trail_len - 1, n = 0; i >= 0 && n < window; i--, n++) {
    if (ref_eq(P.trail[i], r)) {
      return true;
    }
  }
  return false;
}

static TrackRef pick_random(const Playlist *pl, TrackRef from) {
  TrackRef r = { pl->id, pl->tracks[0].id };
  if (pl->len == 1) {
    return r;
  }
  int window = (int)(pl->len - 1 < RECENT_AVOID ? pl->len - 1 : RECENT_AVOID);
  for (int i = 0; i < RANDOM_TRIES; i++) {
    r.track = pl->tracks[rng_u32() % pl->len].id;
    if (r.track != from.track && !recently_played(r, window)) {
      break;
    }
  }
  return r;
}

static size_t hint_for(TrackRef from, size_t fallback) {
  return (P.has_cur && ref_eq(from, P.cur)) ? P.cur_idx : fallback;
}

static bool compute_next(TrackRef from, size_t hint, TrackRef *out, Nav *nav) {
  *nav = NAV_NEW;
  bool moved = P.sel_stamp > P.decided_stamp;
  P.decided_stamp = P.sel_stamp;
  if (M.follow && moved && M.sel_pl && M.sel_track) {
    TrackRef sel = { M.sel_pl, M.sel_track };
    if (!ref_eq(sel, from) && ref_valid(sel)) {
      *out = sel;
      return true;
    }
  }
  Playlist *pl = model_find(from.pl);
  if (!pl || pl->len == 0) {
    return false;
  }
  if (model_effective_order(pl) == ORDER_RANDOM) {
    if (P.trail_pos < P.trail_len - 1 && ref_valid(P.trail[P.trail_pos + 1])) {
      *out = P.trail[P.trail_pos + 1];
      *nav = NAV_FWD;
      return true;
    }
    *out = pick_random(pl, from);
    return true;
  }
  ssize_t idx = playlist_index(pl, from.track);
  int64_t next = (idx >= 0 ? idx : (int64_t)hint - 1) + 1;
  if (next >= (int64_t)pl->len) {
    if (model_effective_order(pl) == ORDER_NOLOOP) {
      return false;
    }
    next = 0;
  }
  *out = (TrackRef){ pl->id, pl->tracks[next].id };
  return true;
}

static void send_next(TrackRef from, size_t hint, uint64_t token) {
  TrackRef nxt;
  Nav nav;
  AudioItem it;
  if (compute_next(from, hint, &nxt, &nav) && make_item(nxt, nav, &it)) {
    audio_next(&it, token);
  } else {
    audio_next(NULL, token);
  }
}

static void on_started(const AudioEvent *ev) {
  Sent *s = lookup(ev->token);
  if (!s || ev->token == P.started_token || (P.pending_token && ev->token < P.pending_token)) {
    return;
  }
  P.started_token = ev->token;
  P.pending_token = 0;
  P.cur = s->ref;
  P.has_cur = true;
  P.cur_idx = s->idx;
  P.errors = 0;
  if (P.state == PS_STOPPED) {
    P.state = PS_PLAYING;
  }
  snprintf(P.format, sizeof(P.format), "%s", ev->text);
  if (s->nav != NAV_SEEK) {
    P.icy[0] = '\0';
    trail_record(s->ref, s->nav);
  }
  changed();
}

static void on_need_next(const AudioEvent *ev) {
  Sent *s = lookup(ev->token);
  if (!s) {
    audio_next(NULL, ev->token);
    return;
  }
  send_next(s->ref, hint_for(s->ref, s->idx), ev->token);
}

static void on_error(const AudioEvent *ev) {
  Sent *s = lookup(ev->token);
  Playlist *pl = s ? model_find(s->ref.pl) : NULL;
  P.errors++;
  LOG("playback error: %s", ev->text);
  if (!s || !pl || P.errors > pl->len || P.errors > MAX_CONSECUTIVE_ERRORS) {
    audio_next(NULL, ev->token);
    return;
  }
  send_next(s->ref, s->idx, ev->token);
}

static void on_output_error(const AudioEvent *ev) {
  if (P.pending_token && ev->token < P.pending_token) {
    return;
  }
  LOG("output %s failed, stopped", ev->text);
  Sent *s = lookup(ev->token);
  if (s) {
    P.cur = s->ref;
    P.cur_idx = s->idx;
    P.has_cur = true;
  }
  P.state = PS_STOPPED;
  P.started_token = 0;
  P.pending_token = 0;
  P.errors = 0;
  P.has_resume = P.has_cur;
  P.resume_ms = ev->pos_ms;
  changed();
}

void player_on_audio(void *ud) {
  (void)ud;
  audio_ack_events();
  AudioEvent ev;
  while (audio_poll_event(&ev)) {
    switch (ev.type) {
      case AEV_STARTED:
        on_started(&ev);
        break;
      case AEV_NEED_NEXT:
        on_need_next(&ev);
        break;
      case AEV_ERROR:
        on_error(&ev);
        break;
      case AEV_OUTPUT_ERROR:
        on_output_error(&ev);
        break;
      case AEV_STOPPED:
        if (P.state != PS_STOPPED) {
          P.state = PS_STOPPED;
          P.started_token = 0;
          P.pending_token = 0;
          changed();
        }
        break;
      case AEV_ICY:
        snprintf(P.icy, sizeof(P.icy), "%s", ev.text);
        server_event("player");
        break;
    }
  }
}

void player_watchdog(void) {
  if (!P.pending_token || mono_ms() - P.pending_since < START_TIMEOUT_MS) {
    return;
  }
  LOG("audio not responding, stopped");
  audio_stop();
  P.state = PS_STOPPED;
  P.pending_token = 0;
  P.started_token = 0;
  P.has_resume = P.has_cur;
  P.resume_ms = P.pending_pos;
  changed();
}

void player_init(const StorePlaying *resume) {
  memset(&P, 0, sizeof(P));
  audio_set_volume(M.volume);
  if (resume && resume->valid && ref_valid((TrackRef){ resume->pl, resume->track })) {
    P.cur = (TrackRef){ resume->pl, resume->track };
    P.has_cur = true;
    P.has_resume = true;
    P.resume_ms = resume->pos_ms;
    player_sync_hint(resume->pl);
  }
}

static uint32_t current_pos(void) {
  if (P.state == PS_STOPPED) {
    return P.has_resume ? P.resume_ms : 0;
  }
  if (P.pending_token) {
    return P.pending_pos;
  }
  return audio_position_ms();
}

void player_status(PlayerStatus *st) {
  st->state = P.state;
  st->has_track = P.has_cur;
  st->cur = P.cur;
  st->pos_ms = current_pos();
  st->format = P.state != PS_STOPPED ? P.format : "";
  st->icy = P.state != PS_STOPPED ? P.icy : "";
}

StorePlaying player_store_state(void) {
  StorePlaying s = { 0 };
  if (P.has_cur && (P.state != PS_STOPPED || P.has_resume)) {
    s.valid = true;
    s.pl = P.cur.pl;
    s.track = P.cur.track;
    s.pos_ms = current_pos();
  }
  return s;
}

bool player_play(TrackRef ref) {
  return start(ref, 0, NAV_NEW, false);
}

bool player_play_default(void) {
  if (P.state == PS_PAUSED) {
    return player_resume();
  }
  if (P.state == PS_PLAYING) {
    return true;
  }
  if (P.has_resume && start(P.cur, P.resume_ms, NAV_NEW, false)) {
    return true;
  }
  if (M.sel_pl && M.sel_track && start((TrackRef){ M.sel_pl, M.sel_track }, 0, NAV_NEW, false)) {
    return true;
  }
  Playlist *pl = model_find(M.sel_pl);
  if (pl && pl->len) {
    return start((TrackRef){ pl->id, pl->tracks[0].id }, 0, NAV_NEW, false);
  }
  return false;
}

bool player_pause(void) {
  if (P.state != PS_PLAYING) {
    return false;
  }
  audio_pause(true);
  P.state = PS_PAUSED;
  changed();
  return true;
}

bool player_resume(void) {
  if (P.state != PS_PAUSED) {
    return false;
  }
  audio_pause(false);
  P.state = PS_PLAYING;
  changed();
  return true;
}

void player_stop(void) {
  audio_stop();
  P.state = PS_STOPPED;
  P.has_resume = false;
  P.started_token = 0;
  P.pending_token = 0;
  changed();
}

bool player_next(void) {
  if (!P.has_cur) {
    return player_play_default();
  }
  TrackRef nxt;
  Nav nav;
  if (compute_next(P.cur, P.cur_idx, &nxt, &nav)) {
    return start(nxt, 0, nav, false);
  }
  player_stop();
  return true;
}

bool player_prev(void) {
  if (!P.has_cur) {
    return player_play_default();
  }
  Playlist *pl = model_find(P.cur.pl);
  if (!pl || pl->len == 0) {
    return false;
  }
  if (model_effective_order(pl) == ORDER_RANDOM && P.trail_pos > 0 && ref_valid(P.trail[P.trail_pos - 1])) {
    return start(P.trail[P.trail_pos - 1], 0, NAV_BACK, false);
  }
  ssize_t idx = playlist_index(pl, P.cur.track);
  int64_t prev = (idx >= 0 ? idx : (int64_t)P.cur_idx) - 1;
  if (prev < 0) {
    prev = (int64_t)pl->len - 1;
  }
  return start((TrackRef){ pl->id, pl->tracks[prev].id }, 0, NAV_NEW, false);
}

bool player_seek(int64_t ms, bool relative) {
  if (!P.has_cur) {
    return false;
  }
  Playlist *pl = model_find(P.cur.pl);
  ssize_t idx = pl ? playlist_index(pl, P.cur.track) : -1;
  if (idx < 0 || (pl->tracks[idx].flags & TRACK_STREAM)) {
    return false;
  }
  uint32_t dur = pl->tracks[idx].dur_ms;
  int64_t base = relative ? (int64_t)player_store_state().pos_ms + ms : ms;
  if (base < 0) {
    base = 0;
  }
  if (dur && base > dur) {
    base = dur;
  }
  if (P.state == PS_STOPPED) {
    if (!P.has_resume) {
      return false;
    }
    P.resume_ms = (uint32_t)base;
    changed();
    return true;
  }
  return start(P.cur, (uint32_t)base, NAV_SEEK, P.state == PS_PAUSED);
}

void player_set_volume(int volume) {
  M.volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
  audio_set_volume(M.volume);
  changed();
}

void player_set_output(const char *id) {
  if (M.output && strcmp(M.output, id) == 0) {
    return;
  }
  free(M.output);
  M.output = xstrdup(id);
  char *pcm = outputs_pcm(id);
  audio_set_device(pcm);
  free(pcm);
  changed();
}

void player_select(TrackRef ref) {
  M.sel_pl = ref.pl;
  M.sel_track = ref.track;
  P.sel_stamp++;
  M.dirty = true;
  server_event("player");
  app_changed();
}

void player_sync_hint(uint32_t pl_id) {
  if (!P.has_cur || P.cur.pl != pl_id) {
    return;
  }
  Playlist *pl = model_find(pl_id);
  ssize_t idx = pl ? playlist_index(pl, P.cur.track) : -1;
  if (idx >= 0) {
    P.cur_idx = (size_t)idx;
  }
}

void player_playlist_deleted(uint32_t pl_id) {
  if (P.has_cur && P.cur.pl == pl_id && P.state == PS_STOPPED) {
    P.has_cur = false;
    P.has_resume = false;
  }
}
