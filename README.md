# mpzd

mpzd is a music player daemon for [mpz](https://github.com/olegantonyan/mpz). It works the same way as mpz: you have one library folder, make playlists from its folders and play tracks from a playlist. It is intended to run on small Linux boards.

The protocol is described in [docs/protocol.md](docs/protocol.md).

## Build

You need FFmpeg (`libavformat libavcodec libavutil libswresample`), ALSA, zlib, CMake and a C17 compiler. PipeWire (`libpipewire-0.3`) is optional.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Add `-DMPZD_ASAN=ON` to build with AddressSanitizer and UBSan, or `-DMPZD_PIPEWIRE=OFF` to build without PipeWire.

On embedded targets you can use a smaller FFmpeg build, for example:

```
./configure --disable-everything --disable-programs --disable-doc --disable-avdevice --disable-avfilter --disable-swscale --enable-swresample --enable-protocol=file,http,https --enable-demuxer=flac,mp3,ogg,mov,wav,aiff,ape,wv,matroska,aac,asf,dsf,tta,mpc,mpc8 --enable-parser=flac,mpegaudio,vorbis,opus,aac --enable-decoder=flac,mp3,mp3float,vorbis,opus,aac,alac,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,pcm_s16be,pcm_s24be,ape,wavpack,wmav2,wmalossless,dsd_lsbf,dsd_msbf,tta,mpc7,mpc8
```

## Porting

`src/` except `src/linux/` is portable C (pthreads, POSIX files, BSD sockets, `poll`); `src/core.cmake` lists its sources. A platform implements these headers:

| Header | Linux |
|---|---|
| `player/decoder.h` | `linux/decoder_ffmpeg.c` |
| `player/sink.h` | `linux/sink_alsa.c` |
| `library/tags.h` | `linux/tags_ffmpeg.c` |
| `player/outputs.h` (`outputs_list`, `outputs_valid`, `outputs_pcm`) | `linux/outputs.c` |
| `util/wake.h` | `linux/wake.c` (eventfd) |

and a `main` that wires them to `util/loop.h`, as `linux/main.c` does. ESP-IDF builds the core with `_GNU_SOURCE`, `LWIP_NETDB_HAS_GAI_STRERROR=1` plus a `gai_strerror()`, and the `espressif/zlib` component.

## Run

```
mpzd [-c config] [-l library_dir] [-d state_dir] [-b bind_addr] [-p port] [-s unix_socket] [-a pipewire|alsa] [-o output]
```

The library folder is required (`-l` or `library` in the config file). mpzd rejects paths outside of it. For several library folders, run one mpzd per folder, each with its own port (or socket) and state directory.

By default mpzd listens on `127.0.0.1:6601`, plays to the `default` device and keeps its state in `$XDG_STATE_HOME/mpzd`. Use `-p ''` to turn TCP off. With ALSA, use a plug device such as `default` or `plughw:...` unless your hardware supports every sample rate you play.

The config file has one `key value` per line. Command line flags take priority.

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

## Outputs

The output can be changed at runtime with the `output` command, and the choice is saved.

With `-a pipewire` (the default when built with PipeWire), the outputs are the PipeWire sinks and `default`, which follows the PipeWire default sink. mpzd has to run in the user session in this mode.

With `-a alsa`, the outputs are the `output` lines from the config file and the `-o`/`device` value.

## State

mpzd stores its state in `state` and `playlists/<id>.tsv.gz` (gzipped TSV, you can read it with `zcat`). It saves 2 s after a change and on SIGTERM or SIGINT. SIGHUP saves right away. After a restart, `play` continues from the saved position.

## Errors

Missing or broken files are skipped. If many files fail in a row, playback stops. Opening or reading a file times out after 10 s (15 s for streams), and `stop` or `play` cancels a slow open.

If the library is unavailable, for example when the SD card is removed, `ls` and `pl_new` return errors until it comes back. A scan that makes no progress for 30 s is cancelled.

If the output fails or doesn't accept data for 5 s, playback stops and the position is kept. Switch to a working output and run `play` to continue.

State is written in a separate thread, so a slow disk does not block clients. Failed writes are retried every 10 s.

## Example

```
$ mpzd -l /music &
$ nc localhost 6601
mpzd 1
pl_new - "/music/Some Artist/2001 - Album"
play 1
status
```
