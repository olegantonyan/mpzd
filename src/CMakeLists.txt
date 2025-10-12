set(CMAKE_C_STANDARD 17)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED mpv)
pkg_check_modules(CJSON REQUIRED libcjson)

set(PROJECT_SOURCES
  main.c
  mediaplayer/mediaplayer.c
  rpc/server.c
  rpc/protocol.c
  commands/commands.c
)

add_executable(${CMAKE_PROJECT_NAME} ${PROJECT_SOURCES})

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../generated)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${MPV_INCLUDE_DIRS})
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${MPV_LIBRARIES})
target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE ${MPV_CFLAGS_OTHER})

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${CJSON_LIBRARIES})
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CJSON_INCLUDE_DIRS})
target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE ${CJSON_CFLAGS_OTHER})
