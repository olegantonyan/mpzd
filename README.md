# mpzd

Headless music daemon following [mpz](https://github.com/olegantonyan/mpz)'s model: a library folder, playlists made from folders, and playback straight from a playlist (no queue, no database). Aimed at small Linux boards.

Protocol: [docs/protocol.md](docs/protocol.md).

## Build

Dependencies: FFmpeg (`libavformat libavcodec libavutil libswresample`), ALSA, zlib, PipeWire (`libpipewire-0.3`, optional), CMake, a C17 compiler.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

`-DMPZD_ASAN=ON` builds with AddressSanitizer and UBSan. `-DMPZD_PIPEWIRE=OFF` builds without PipeWire (ALSA only).

For embedded targets, a minimal FFmpeg keeps the footprint small, for example:

```
./configure --disable-everything --disable-programs --disable-doc --disable-avdevice --disable-avfilter --disable-swscale --enable-swresample --enable-protocol=file,http,https --enable-demuxer=flac,mp3,ogg,mov,wav,aiff,ape,wv,matroska,aac,asf,dsf,tta,mpc,mpc8 --enable-parser=flac,mpegaudio,vorbis,opus,aac --enable-decoder=flac,mp3,mp3float,vorbis,opus,aac,alac,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,pcm_s16be,pcm_s24be,ape,wavpack,wmav2,wmalossless,dsd_lsbf,dsd_msbf,tta,mpc7,mpc8
```

## Run

```
mpzd [-c config] [-l library_dir] [-d state_dir] [-b bind_addr] [-p port] [-s unix_socket] [-a pipewire|alsa] [-o output]
```

One library folder per instance, required: `-l` or `library` in the config file. Paths outside it are rejected.

Defaults: `127.0.0.1:6601`, ALSA device `default`, state in `$XDG_STATE_HOME/mpzd`. An empty `-p ''` disables TCP. Use a plug device (`default`, `plughw:...`) unless the hardware supports every sample rate you play.

Outputs, switchable at runtime with `output` (the choice is kept in the state):

- `audio pipewire` / `-a pipewire` (default when built with PipeWire): the list is PipeWire's audio sinks, read on each `outputs`; `default` follows PipeWire's default sink. Playback goes through ALSA's `pipewire` PCM, so mpzd must run in the user's session.
- `audio alsa` / `-a alsa` (default without PipeWire, or to switch it off): the list is the `output <alsa_name> [label]` config lines plus the `-o`/`device` one. Nothing else is accepted.

Config file (`key value` lines, CLI flags win):

```
library /music
bind 0.0.0.0
port 6601
socket /run/mpzd.sock
device plughw:0,0
output plughw:0,0 Speakers
output plughw:1,0 USB DAC
state_dir /var/lib/mpzd
max_clients 16
```

State is written atomically (`state` plus gzipped TSV `playlists/<id>.tsv.gz`, readable with `zcat`) within 2 s of a change and on SIGTERM/SIGINT; SIGHUP flushes immediately. Plain `playlists/<id>.tsv` from earlier builds is loaded and converted on the next save. After a restart while playing, `play` resumes at the saved position.

Failures don't stall the daemon:

- **Files:** a missing or unreadable file is skipped, and playback stops after a run of failures. File opens and reads time out after 10 s, streams after 15 s, and `stop` or `play` interrupts them.
- **Library:** an unavailable library (card removed) only makes `ls`/`pl_new` fail until it is back. A scan that makes no progress for 30 s is abandoned.
- **Output:** a failing or stalled output (5 s) stops playback and keeps the position, so `play` resumes after switching outputs.
- **Disk:** saving happens on a separate thread. Failed writes are retried every 10 s.

## Quick try

```
$ mpzd -l /music &
$ nc localhost 6601
mpzd 1
pl_new - "/music/Some Artist/2001 - Album"
play 1
status
```
