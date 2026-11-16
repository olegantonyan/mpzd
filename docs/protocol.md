# mpzd protocol v1

The protocol uses UTF-8 text lines over TCP (default `127.0.0.1:6601`) or a unix socket. You can talk to the server with `nc` or `socat`.

## Format

On connect the server sends `mpzd 1`.

A request is `command arg ...\n`, with arguments separated by spaces. An argument that contains spaces goes in `"..."`, with `"` and `\` escaped by `\`. Lines are limited to 8192 bytes.

A reply has zero or more `key: value` lines and ends with `ok` or `err <code> <message>`. In lists, each record starts with its id key (`playlist:`, `track:`, `dir:` or `file:`). Empty fields are left out, and newlines inside values are sent as spaces.

Clients can send several requests without waiting for replies. Replies come back in the same order.

| Error code | Meaning |
|---|---|
| `1` | unknown command or syntax error |
| `2` | bad argument |
| `3` | not found |
| `4` | I/O error |
| `5` | invalid state |

`ls`, `pl_new`, `pl_add`, `outputs` and `output <id>` run in the background and don't block other clients. They return `err 4 timed out` if the disk doesn't answer in 5 s (PipeWire in 3 s), and `err 4 busy` if too many earlier requests are still waiting.

The server closes dead TCP connections after about 30 s.

## Events

After `watch on` the server sends `! ...` lines between replies. Events only say what changed, so the client has to request the new data.

| Event | Sent when |
|---|---|
| `! player` | playback state, track, position (after a seek), volume, output, order, follow, selection or stream title changed |
| `! playlist <id>` | tracks of the playlist changed |
| `! playlists` | the list of playlists, a name or the order changed |
| `! scan <id> done` | a folder scan for playlist `<id>` finished |

## Ids

Playlist and track ids don't change after a restart. A track id is unique within its playlist, so the same file in two playlists has two different ids. Commands that change tracks use ids instead of positions.

## Commands

### Meta

| Command | Reply |
|---|---|
| `ping` | |
| `version` | `version`, `protocol` |
| `commands` | `command` for each command |
| `watch on\|off` | |
| `close` | closes the connection after the reply |

### Library

Paths are absolute and have to be inside the library folder. `ls` shows folders first, then supported files (audio, `cue`, `m3u`, `m3u8`, `pls`). Hidden files are skipped.

| Command | Reply |
|---|---|
| `library` | `library` |
| `ls [<path>] [name\|date\|-date]` | `dir` or `file` for each entry, as names. Lists the library folder if `<path>` is omitted |

### Playlists

| Command | Reply / effect |
|---|---|
| `playlists` | for each playlist: `playlist`, `name`, `count`, `order`, `scanning` (number of pending scans) |
| `pl_new <name\|-> <path\|url>...` | `playlist`. The playlist is created right away and filled in the background (see `! scan`). With `-` the name is made from the paths |
| `pl_add <pl> [at <pos>] <path\|url>...` | adds tracks to the end or at `pos`, in the background |
| `pl_tracks <pl> [from [count]]` | `total`, then the tracks. `count` is 1000 by default |
| `pl_track <pl> <track>` | `pos` and the track |
| `pl_rm <pl> <track>...` | `removed` |
| `pl_move <pl> <pos> <track>...` | `moved`. The tracks are moved together to `pos` and keep their order |
| `pl_copy <src> <dst> [at <pos>] <track>...` | `copied`. To move tracks to another playlist, use `pl_copy` and then `pl_rm` |
| `pl_rename <pl> <name>` | |
| `pl_del <pl>` | |
| `pl_pos <pl> <pos>` | moves the playlist in the list |
| `pl_order <pl> inherit\|seq\|random\|noloop` | sets the playlist order. `inherit` uses the global order |
| `pl_sort <pl> [criteria]` | sorts the playlist once. Criteria are keys joined by `/`, and `-` before a key reverses it. Keys: `ARTIST ALBUMARTIST ALBUM YEAR TRACKNUMBER DISCNUMBER FILENAME TITLE DIRECTORY`. Default: `YEAR/ALBUM/DIRECTORY/DISCNUMBER/TRACKNUMBER/FILENAME/TITLE` |
| `pl_reload <pl>` | reads the tags again, in the background |

Folders are scanned recursively. If a cue sheet refers to an audio file, the cue tracks are added instead of the file. Playlist files found inside folders are ignored, but an `m3u` or `pls` passed directly is added in its own order. Everything else is sorted with the default criteria, separately for each path.

Track fields: `track`, `file`, `begin_ms` and `end_ms` (cue tracks only; the last track of a file has no `end_ms`), `stream: 1` for URLs, `dur_ms`, `artist`, `album_artist`, `album`, `title` (the file name or URL if there is no tag), `year`, `number`, `disc`.

### Player

| Command | Reply / effect |
|---|---|
| `status` | `state` (`play`, `pause` or `stop`). If there is a current track: `playlist`, the track fields, `pos_ms`, `format`, `icy`. Always: `volume`, `output`, `order`, `follow`, `sel_playlist`, `sel_track` |
| `play` | unpauses if paused. Otherwise plays from the saved position, the selected track, or the first track of the selected playlist, whichever exists first |
| `play <pl> [<track>]` | plays the track, or the first track of the playlist |
| `pause`, `toggle`, `stop` | |
| `next`, `prev` | |
| `seek <sec\|+sec\|-sec>` | seconds, fractions allowed. `+` and `-` seek relative to the current position |
| `volume [<0-100>\|+n\|-n]` | `volume` |
| `outputs` | for each output: `output` (id), `desc`, and `active` for the current one |
| `output [<id>]` | `output`. Switches the output without stopping playback. The id has to be one from `outputs` |
| `order [seq\|random\|noloop]` | sets the global order |
| `select <pl> [<track>]` | sets the selection (the cursor and current playlist in mpz) |
| `follow [on\|off]` | makes playback follow the selection |

mpzd has no play queue and plays tracks from the current playlist. When a track ends, the next one is chosen like this:

1. If `follow` is on and the selection has changed, play the selected track.
2. Otherwise use the playlist order, or the global order if the playlist has `inherit`. `seq` starts over after the last track, `noloop` stops there, and `random` skips the last `min(n-1, 16)` played tracks.

In random order, `prev` goes back through the last 64 played tracks and `next` goes forward through them again.

If the playing track is removed from the playlist, it plays to the end and the next track is taken from its old position.

Tracks with the same sample rate, number of channels and sample format play without a gap.
