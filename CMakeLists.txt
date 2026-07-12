cmake_minimum_required(VERSION 3.10)
project(c_examples)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_INSTALL_PREFIX ${CMAKE_SOURCE_DIR}/dist CACHE PATH "Install prefix" FORCE)


find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDRM REQUIRED libdrm)
pkg_check_modules(LIBGPIO REQUIRED libgpiod)

# 共享库：所有例程都链接这个 target
add_library(c_examples_libs STATIC
  lib/drm_warpper.c
  lib/log.c
  lib/spsc_queue.c
  lib/crrefont.cpp
  lib/RREFont/RREFont.cpp
  lib/keyinput.c
  lib/fbdraw.c
  lib/fbdrawttf.c
  lib/i2c.c
  lib/spi.c
  lib/uart.c
  lib/ipc_client.c
  lib/ipc_common.c
  lib/uuid.c
)

target_include_directories(c_examples_libs PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/lib
  ${CMAKE_CURRENT_SOURCE_DIR}/lib/RREFont
  ${LIBDRM_INCLUDE_DIRS}
)

target_link_libraries(c_examples_libs PUBLIC
  ${LIBDRM_LIBRARIES}
  pthread
  ${LIBGPIO_LIBRARIES}
  m
)

add_subdirectory(examples)

