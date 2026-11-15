# mpzd protocol v1

Line-based text over TCP (default `127.0.0.1:6601`) or a unix socket. UTF-8. Usable with `nc`/`socat`.

## Framing

- On connect the server sends `mpzd 1`.
- Request: `command arg ...\n`. Arguments are separated by spaces; quote an argument with `"..."` when it contains spaces, escaping `"` and `\` with `\`. Max line length 8192.
- Reply: zero or more `key: value` lines, then `ok` or `err <code> <message>`.
- Lists are sequences of records; each record starts with its id key (`playlist:`, `track:`, `dir:`/`file:`). Empty fields are omitted. Newlines inside values are sent as spaces.
- Requests may be pipelined; replies come in order.

Error codes: `1` unknown command / syntax, `2` bad argument, `3` not found, `4` I/O, `5` invalid state.

## Events

After `watch on` the server sends `! ...` lines between replies, never inside one. Events carry no data; clients re-fetch.

| Event | Meaning |
|---|---|
| `! player` | Playback state, track, position jump, volume, order, follow, selection or ICY title changed |
| `! playlist <id>` | Tracks of a playlist changed (count included) |
| `! playlists` | Playlist list or a playlist's name/order changed |
| `! scan <id> done` | A folder scan for playlist `<id>` finished |

## Ids

Playlist ids and track ids are stable across restarts. Track ids are unique within their playlist; the same file in two playlists has two ids. Commands that modify tracks take ids, not positions.

## Commands

### Meta

| Command | Reply |
|---|---|
| `ping` | |
| `version` | `version`, `protocol` |
| `commands` | `command` per command |
| `watch on\|off` | |
| `close` | closes after the reply |

### Library

Paths are absolute and must be inside the library: one folder per daemon, set by `-l` or `library` in the config file. `ls` lists only folders and supported files (audio, `cue`, `m3u`, `m3u8`, `pls`); hidden entries are skipped. Folders come first.

| Command | Reply |
|---|---|
| `library` | `library` |
| `ls [<path>] [name\|date\|-date]` | `dir` / `file` per entry (names, not paths). `<path>` defaults to the library |

### Playlists

| Command | Reply / effect |
|---|---|
| `playlists` | per playlist: `playlist`, `name`, `count`, `order`, `scanning` (pending scans) |
| `pl_new <name\|-> <path\|url>...` | `playlist`. Created immediately, filled asynchronously (`! scan`). `-` names it from the paths relative to the library |
| `pl_add <pl> [at <pos>] <path\|url>...` | Async append or insert |
| `pl_tracks <pl> [from [count]]` | `total`, then track records. `count` defaults to 1000 |
| `pl_track <pl> <track>` | `pos` and the track record |
| `pl_rm <pl> <track>...` | `removed` |
| `pl_move <pl> <pos> <track>...` | `moved`. Tracks move as a block, keeping their order, to start at `pos` |
| `pl_copy <src> <dst> [at <pos>] <track>...` | `copied`. A move between playlists is `pl_copy` followed by `pl_rm` |
| `pl_rename <pl> <name>` | |
| `pl_del <pl>` | |
| `pl_pos <pl> <pos>` | Moves the playlist within the list |
| `pl_order <pl> inherit\|seq\|random\|noloop` | Per-playlist order; `inherit` uses the global order |
| `pl_sort <pl> [criteria]` | One-off sort. Criteria: keys joined by `/`, `-` prefix reverses; keys `ARTIST ALBUMARTIST ALBUM YEAR TRACKNUMBER DISCNUMBER FILENAME TITLE DIRECTORY`. Default `YEAR/ALBUM/DIRECTORY/DISCNUMBER/TRACKNUMBER/FILENAME/TITLE` |
| `pl_reload <pl>` | Re-reads tags asynchronously |

Folders are scanned recursively. Audio files referenced by a cue sheet are replaced by the cue's tracks. Playlist files inside folders are ignored; an `m3u`/`pls` given directly is expanded in file order. Everything else is sorted by the default criteria per added path.

Track record: `track`, `file`, `begin_ms`/`end_ms` (cue only, `end_ms` absent for the last track of a file), `stream: 1` (URL), `dur_ms`, `artist`, `album_artist`, `album`, `title` (falls back to the file name or URL), `year`, `number`, `disc`.

### Player

| Command | Reply / effect |
|---|---|
| `status` | `state` (`play\|pause\|stop`); when there is a current track: `playlist`, the track record, `pos_ms`, `format`, `icy`; always: `volume`, `order`, `follow`, `sel_playlist`, `sel_track` |
| `play` | Resume if paused; otherwise the resume point saved at shutdown; otherwise the selected track; otherwise the first track of the selected playlist |
| `play <pl> [<track>]` | Plays the track, or the playlist's first track |
| `pause`, `toggle`, `stop` | |
| `next`, `prev` | |
| `seek <sec\|+sec\|-sec>` | Fractional seconds; `+`/`-` is relative |
| `volume [<0-100>\|+n\|-n]` | `volume` |
| `order [seq\|random\|noloop]` | Global order |
| `select <pl> [<track>]` | The selection (mpz's cursor and current playlist) |
| `follow [on\|off]` | Playback follows the selection |

There is no queue: the playing playlist is the queue. After a track ends:

1. If `follow` is on and the selection changed since the last decision, play the selected track.
2. Otherwise use the playing playlist's order (or the global one for `inherit`): `seq` wraps, `noloop` stops after the last track, `random` avoids the last `min(n-1, 16)` tracks played.

`prev` in random order walks back through the last 64 played tracks, and `next` replays forward through them. Removing the playing track lets it finish; the next track is taken from its former position. Consecutive tracks with the same sample rate, channel count and sample format play gaplessly.
