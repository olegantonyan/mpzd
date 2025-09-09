set(CMAKE_C_STANDARD 17)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED mpv)

set(PROJECT_SOURCES
  main.c
  mediaplayer/mediaplayer.c
)

add_executable(${CMAKE_PROJECT_NAME} ${PROJECT_SOURCES})

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${MPV_INCLUDE_DIRS})
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${MPV_LIBRARIES})
target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE ${MPV_CFLAGS_OTHER})
