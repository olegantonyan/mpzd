# mpzd

Headless music daemon following [mpz](https://github.com/olegantonyan/mpz)'s model: library folders, playlists made from folders, and playback straight from a playlist (no queue, no database). Aimed at small Linux boards.

Protocol: [docs/protocol.md](docs/protocol.md).

## Build

Dependencies: FFmpeg (`libavformat libavcodec libavutil libswresample`), ALSA, CMake, a C17 compiler.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

`-DMPZD_ASAN=ON` builds with AddressSanitizer and UBSan.

For embedded targets, a minimal FFmpeg keeps the footprint small, for example:

```
./configure --disable-everything --disable-programs --disable-doc --disable-avdevice --disable-avfilter --disable-swscale --enable-swresample --enable-protocol=file,http,https --enable-demuxer=flac,mp3,ogg,mov,wav,aiff,ape,wv,matroska,aac,asf,dsf,tta,mpc,mpc8 --enable-parser=flac,mpegaudio,vorbis,opus,aac --enable-decoder=flac,mp3,mp3float,vorbis,opus,aac,alac,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,pcm_s16be,pcm_s24be,ape,wavpack,wmav2,wmalossless,dsd_lsbf,dsd_msbf,tta,mpc7,mpc8
```

## Run

```
mpzd [-c config] [-l library_dir]... [-d state_dir] [-b bind_addr] [-p port] [-s unix_socket] [-o alsa_device]
```

Library folders are given with `-l` (repeatable) or `library` lines in the config file; `-l` replaces the config file's list. Paths outside them are rejected.

Defaults: `127.0.0.1:6601`, ALSA device `default`, state in `$XDG_STATE_HOME/mpzd`. An empty `-p ''` disables TCP. Use a plug device (`default`, `plughw:...`) unless the hardware supports every sample rate you play.

Config file (`key value` lines, CLI flags win):

```
library /music
library /mnt/usb/music
bind 0.0.0.0
port 6601
socket /run/mpzd.sock
device plughw:0,0
state_dir /var/lib/mpzd
max_clients 16
```

State is written atomically (`state` plus `playlists/<id>.tsv`) within 2 s of a change and on SIGTERM/SIGINT; SIGHUP flushes immediately. After a restart while playing, `play` resumes at the saved position.

## Quick try

```
$ mpzd -l /music &
$ nc localhost 6601
mpzd 1
pl_new - "/music/Some Artist/2001 - Album"
play 1
status
```
