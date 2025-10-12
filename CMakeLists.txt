cmake_minimum_required(VERSION 3.10)
project(mpzd VERSION 0.0.1)

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/version.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h"
  @ONLY
)

add_subdirectory(src)
