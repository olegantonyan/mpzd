#include "app.h"
#include "model/model.h"
#include "player/player.h"
#include "proto/handlers.h"
#include "proto/proto.h"

#include <stdlib.h>
#include <string.h>

static bool status(int argc, char **argv) {
  PlayerStatus st;
  player_status(&st);
  r_str("state", play_state_name(st.state));
  if (st.has_track) {
    r_u32("playlist", st.cur.pl);
    Playlist *p = model_find(st.cur.pl);
    ssize_t idx = p ? playlist_index(p, st.cur.track) : -1;
    if (idx >= 0) {
      r_track(&p->tracks[idx]);
    } else {
      r_u32("track", st.cur.track);
    }
    r_u32("pos_ms", st.pos_ms);
    r_opt("format", st.format);
    r_opt("icy", st.icy);
  }
  r_u32("volume", (uint32_t)M.volume);
  r_str("order", order_name(M.order));
  r_u32("follow", M.follow ? 1 : 0);
  r_opt_u32("sel_playlist", M.sel_pl);
  r_opt_u32("sel_track", M.sel_track);
  return true;
}

static bool play(int argc, char **argv) {
  if (argc == 0) {
    return player_play_default() || r_err(E_STATE, "nothing to play");
  }
  uint32_t pl_id;
  if (!arg_u32(argv[0], &pl_id, "playlist id")) {
    return false;
  }
  Playlist *p = model_find(pl_id);
  if (!p) {
    return r_err(E_NOT_FOUND, "no playlist %u", pl_id);
  }
  uint32_t track;
  if (argc > 1) {
    if (!arg_u32(argv[1], &track, "track id")) {
      return false;
    }
  } else if (p->len) {
    track = p->tracks[0].id;
  } else {
    return r_err(E_STATE, "playlist is empty");
  }
  return player_play((TrackRef){ pl_id, track }) || r_err(E_NOT_FOUND, "no track %u", track);
}

static bool pause_cmd(int argc, char **argv) {
  player_pause();
  return true;
}

static bool toggle(int argc, char **argv) {
  PlayerStatus st;
  player_status(&st);
  if (st.state == PS_PLAYING) {
    return player_pause();
  }
  if (st.state == PS_PAUSED) {
    return player_resume();
  }
  return player_play_default() || r_err(E_STATE, "nothing to play");
}

static bool stop(int argc, char **argv) {
  player_stop();
  return true;
}

static bool next(int argc, char **argv) {
  return player_next() || r_err(E_STATE, "nothing to play");
}

static bool prev(int argc, char **argv) {
  return player_prev() || r_err(E_STATE, "nothing to play");
}

static bool seek(int argc, char **argv) {
  const char *s = argv[0];
  bool relative = s[0] == '+' || s[0] == '-';
  char *end;
  double sec = strtod(s, &end);
  if (end == s || *end) {
    return r_err(E_ARG, "invalid position '%s'", s);
  }
  return player_seek((int64_t)(sec * 1000), relative) || r_err(E_STATE, "cannot seek");
}

static bool volume(int argc, char **argv) {
  if (argc == 0) {
    r_u32("volume", (uint32_t)M.volume);
    return true;
  }
  const char *s = argv[0];
  bool relative = s[0] == '+' || s[0] == '-';
  char *end;
  long v = strtol(s, &end, 10);
  if (end == s || *end) {
    return r_err(E_ARG, "invalid volume '%s'", s);
  }
  player_set_volume((int)(relative ? M.volume + v : v));
  r_u32("volume", (uint32_t)M.volume);
  return true;
}

static bool order(int argc, char **argv) {
  if (argc == 0) {
    r_str("order", order_name(M.order));
    return true;
  }
  Order o;
  if (!order_parse(argv[0], &o) || o == ORDER_INHERIT) {
    return r_err(E_ARG, "expected seq|random|noloop");
  }
  M.order = o;
  M.dirty = true;
  server_event("player");
  app_changed();
  return true;
}

static bool select_cmd(int argc, char **argv) {
  uint32_t pl_id;
  uint32_t track = 0;
  if (!arg_u32(argv[0], &pl_id, "playlist id") || (argc > 1 && !arg_u32(argv[1], &track, "track id"))) {
    return false;
  }
  Playlist *p = model_find(pl_id);
  if (!p) {
    return r_err(E_NOT_FOUND, "no playlist %u", pl_id);
  }
  if (track && playlist_index(p, track) < 0) {
    return r_err(E_NOT_FOUND, "no track %u", track);
  }
  player_select((TrackRef){ pl_id, track });
  return true;
}

static bool follow(int argc, char **argv) {
  if (argc == 0) {
    r_u32("follow", M.follow ? 1 : 0);
    return true;
  }
  if (strcmp(argv[0], "on") == 0) {
    M.follow = true;
  } else if (strcmp(argv[0], "off") == 0) {
    M.follow = false;
  } else {
    return r_err(E_ARG, "expected on|off");
  }
  M.dirty = true;
  server_event("player");
  app_changed();
  return true;
}

const Command cmd_player[] = {
  { "status", status, 0, 0 },
  { "play", play, 0, 2 },
  { "pause", pause_cmd, 0, 0 },
  { "toggle", toggle, 0, 0 },
  { "stop", stop, 0, 0 },
  { "next", next, 0, 0 },
  { "prev", prev, 0, 0 },
  { "seek", seek, 1, 1 },
  { "volume", volume, 0, 1 },
  { "order", order, 0, 1 },
  { "select", select_cmd, 1, 2 },
  { "follow", follow, 0, 1 },
  { NULL, NULL, 0, 0 },
};
