find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswresample)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)

include(core.cmake)
add_library(mpzdcore STATIC ${MPZD_CORE_SOURCES})
target_include_directories(mpzdcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(mpzdcore PUBLIC ZLIB::ZLIB Threads::Threads)

add_library(mpzdlinux OBJECT
  linux/decoder_ffmpeg.c
  linux/sink_alsa.c
  linux/tags_ffmpeg.c
  linux/outputs.c
  linux/wake.c
)
target_link_libraries(mpzdlinux PUBLIC mpzdcore PkgConfig::FFMPEG PkgConfig::ALSA)

if(MPZD_PIPEWIRE)
  pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
  target_sources(mpzdlinux PRIVATE linux/pw_sinks.c)
  target_compile_definitions(mpzdlinux PUBLIC MPZD_PIPEWIRE)
  target_link_libraries(mpzdlinux PUBLIC PkgConfig::PIPEWIRE)
endif()

add_executable(mpzd linux/main.c)
target_link_libraries(mpzd PRIVATE mpzdlinux)

install(TARGETS mpzd RUNTIME DESTINATION bin)
