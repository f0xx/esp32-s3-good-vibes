cmake_minimum_required(VERSION 3.20)
project(handshake_host C)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(COMMON_DIR "${REPO_ROOT}/zephyr/app/common")
set(HANDSHAKE_SRC "${REPO_ROOT}/zephyr/app/handshake/src")

option(HOST_TSAN "Build with ThreadSanitizer" ON)
option(HOST_ASAN "Build with AddressSanitizer (mutually exclusive with TSan)" OFF)
option(HOST_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)
option(HOST_ENABLE_SDL "Try to link SDL2 for RGB565 window (auto headless-safe)" ON)
# Flat ESP32 arena. Wrap libc malloc for plain/TSan; never wrap under ASan.
option(HOST_FLAT_HEAP "Map DRAM+PSRAM arena and serve k_malloc from it" ON)
option(HOST_FLAT_HEAP_WRAP "Intercept malloc/realloc/free via --wrap (off with ASan)" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(HOST_SOURCES
  src/main_host.c
  src/host_debug.c
  src/host_latency.c
  src/host_mem.c
  src/kernel_host.c
  src/stubs_host.c
  src/qmi8658_host.c
  src/ble_host.c
  src/wifi_host.c
  src/mt200_host.c
  src/host_nvs.c
  src/host_fb.c
  ${HANDSHAKE_SRC}/imu_pipeline.c
  ${COMMON_DIR}/imu_cal.c
  ${COMMON_DIR}/attitude.c
  ${COMMON_DIR}/walk_distance.c
)

add_executable(handshake_host ${HOST_SOURCES})

target_include_directories(handshake_host PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/include_app
  ${HANDSHAKE_SRC}
  ${COMMON_DIR}
)

target_compile_definitions(handshake_host PRIVATE CONFIG_HOST_MOCK=1)
target_compile_options(handshake_host PRIVATE -Wall -Wextra -Wno-unused-parameter -g -O1 -pedantic -Wno-variadic-macro-arguments-omitted -fstack-protector-strong )
target_link_libraries(handshake_host PRIVATE m pthread)

# Optional SDL2 — never required. Runtime also skips when headless.
set(HOST_SDL_FOUND FALSE)
if(HOST_ENABLE_SDL)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 QUIET sdl2)
  endif()
  if(SDL2_FOUND)
    set(HOST_SDL_FOUND TRUE)
    target_compile_definitions(handshake_host PRIVATE HOST_HAS_SDL2=1)
    target_include_directories(handshake_host PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(handshake_host PRIVATE ${SDL2_LIBRARIES})
    target_compile_options(handshake_host PRIVATE ${SDL2_CFLAGS_OTHER})
    message(STATUS "handshake_host: SDL2 enabled (runtime auto-skips if headless)")
  else()
    message(STATUS "handshake_host: SDL2 not found — fb-only build")
  endif()
endif()

if(HOST_TSAN AND HOST_ASAN)
  message(FATAL_ERROR "HOST_TSAN and HOST_ASAN cannot both be ON")
endif()

if(HOST_ASAN AND HOST_FLAT_HEAP_WRAP)
  message(STATUS "handshake_host: disabling malloc wrap under ASan")
  set(HOST_FLAT_HEAP_WRAP OFF)
endif()

if(HOST_FLAT_HEAP)
  target_compile_definitions(handshake_host PRIVATE HOST_FLAT_HEAP=1)
endif()

if(HOST_FLAT_HEAP AND HOST_FLAT_HEAP_WRAP)
  target_sources(handshake_host PRIVATE src/host_mem_wrap.c)
  target_compile_definitions(handshake_host PRIVATE HOST_FLAT_HEAP_WRAP=1)
  target_link_options(handshake_host PRIVATE
    -Wl,--wrap=malloc
    -Wl,--wrap=calloc
    -Wl,--wrap=realloc
    -Wl,--wrap=free
  )
  message(STATUS "handshake_host: flat heap wrap ON (ESP32 DRAM+PSRAM arena)")
elseif(HOST_FLAT_HEAP)
  message(STATUS "handshake_host: flat heap ON (k_malloc only; libc malloc untouched)")
endif()

if(HOST_TSAN)
  target_compile_options(handshake_host PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(handshake_host PRIVATE -fsanitize=thread)
endif()

if(HOST_ASAN)
  target_compile_options(handshake_host PRIVATE -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(handshake_host PRIVATE -fsanitize=address)
endif()

if(HOST_UBSAN)
  target_compile_options(handshake_host PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
  target_link_options(handshake_host PRIVATE -fsanitize=undefined)
endif()

message(STATUS "handshake_host: ${CMAKE_C_COMPILER_ID} TSan=${HOST_TSAN} ASan=${HOST_ASAN} UBSan=${HOST_UBSAN} SDL=${HOST_SDL_FOUND} flat_heap=${HOST_FLAT_HEAP} wrap=${HOST_FLAT_HEAP_WRAP}")
