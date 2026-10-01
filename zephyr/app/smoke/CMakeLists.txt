cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(waveshare_smoke)

# Prefer space-free west links (same pattern as handshake).
set(_link_common "")
if(DEFINED ENV{ZEPHYR_PROJECT} AND EXISTS "$ENV{ZEPHYR_PROJECT}/waveshare-common/fw_version.h")
  set(_link_common "$ENV{ZEPHYR_PROJECT}/waveshare-common")
elseif(DEFINED ENV{HOME} AND EXISTS "$ENV{HOME}/zephyrproject/waveshare-common/fw_version.h")
  set(_link_common "$ENV{HOME}/zephyrproject/waveshare-common")
endif()
if(_link_common)
  set(COMMON_DIR "${_link_common}")
else()
  get_filename_component(APP_ROOT ${CMAKE_CURRENT_SOURCE_DIR} REALPATH)
  set(COMMON_DIR ${APP_ROOT}/../common)
endif()

target_include_directories(app PRIVATE ${COMMON_DIR})

target_sources(app PRIVATE
  src/main.c
  src/ws2812_gpio38.c
  src/wdt_stubs.c
  ${COMMON_DIR}/renderer.c
  ${COMMON_DIR}/panel_backlight.c
)
