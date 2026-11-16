find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswresample)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)

add_library(mpzdcore STATIC
  app.c
  util/util.c
  util/bg.c
  model/track.c
  model/model.c
  model/sort.c
  library/library.c
  library/tags.c
  library/cue.c
  library/m3u.c
  library/scan.c
  store/store.c
  net/server.c
  proto/proto.c
  proto/cmd_meta.c
  proto/cmd_library.c
  proto/cmd_playlist.c
  proto/cmd_player.c
  player/audio.c
  player/outputs.c
  player/player.c
)

target_include_directories(mpzdcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(mpzdcore PUBLIC PkgConfig::FFMPEG PkgConfig::ALSA ZLIB::ZLIB Threads::Threads)

if(MPZD_PIPEWIRE)
  pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
  target_sources(mpzdcore PRIVATE player/pw_sinks.c)
  target_compile_definitions(mpzdcore PUBLIC MPZD_PIPEWIRE)
  target_link_libraries(mpzdcore PUBLIC PkgConfig::PIPEWIRE)
endif()

add_executable(mpzd main.c)
target_link_libraries(mpzd PRIVATE mpzdcore)

install(TARGETS mpzd RUNTIME DESTINATION bin)
