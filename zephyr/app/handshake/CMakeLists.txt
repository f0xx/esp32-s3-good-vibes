cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(waveshare_handshake)

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Prefer space-free west links. CMake REALPATHs the handshake symlink, so a
# relative ../waveshare-common misses. Spaces in the repo path break the
# Xtensa assembler .file directive.
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

message(STATUS "handshake COMMON_DIR=${COMMON_DIR}")

target_sources(app PRIVATE
  src/main.c
  src/ble_looper.c
  src/ble_imu_gatt.c
  src/ble_net_gatt.c
  src/ble_config_gatt.c
  src/ble_crash_gatt.c
  src/ble_ota_gatt.c
  src/boot_button.c
  src/display_panel.c
  src/imu_pipeline.c
  src/panel_draw.c
  src/panel_font_glcd.c
  ${COMMON_DIR}/renderer.c
  src/mt200_bridge.c
  src/mt200_sample_queue.c
  src/qmi8658.c
  src/scene_live.c
  src/ws2812_gpio38.c
  src/vibro_led.c
  ${COMMON_DIR}/battery_bench.c
  ${COMMON_DIR}/battery_adc_esp32.c
  ${COMMON_DIR}/attitude.c
  ${COMMON_DIR}/battery_monitor.c
  ${COMMON_DIR}/chip_temp.c
  ${COMMON_DIR}/clock_sync.c
  ${COMMON_DIR}/clock_sync_ntp.c
  ${COMMON_DIR}/bist.c
  ${COMMON_DIR}/crash_debug.c
  ${COMMON_DIR}/crash_report.c
  ${COMMON_DIR}/crash_alive.c
  ${COMMON_DIR}/crash_ring_store.c
  ${COMMON_DIR}/crash_rtc_capture.c
  ${COMMON_DIR}/device_config.c
  ${COMMON_DIR}/ota_ab.c
  ${COMMON_DIR}/ota_channel.c
  ${COMMON_DIR}/ota_wifi.c
  ${COMMON_DIR}/soft_reboot.c
  ${COMMON_DIR}/device_profile.c
  ${COMMON_DIR}/flash_safety.c
  ${COMMON_DIR}/floor_calib.c
  ${COMMON_DIR}/imu_cal.c
  ${COMMON_DIR}/net_profile_store.c
  ${COMMON_DIR}/network_manager.c
  ${COMMON_DIR}/panel_backlight.c
  ${COMMON_DIR}/power_manager.c
  ${COMMON_DIR}/radio_scheduler.c
  ${COMMON_DIR}/projection.c
  ${COMMON_DIR}/scene_snapshot.c
  ${COMMON_DIR}/scene_zoom.c
  ${COMMON_DIR}/stall_watchdog.c
  ${COMMON_DIR}/vibro_capture.c
  ${COMMON_DIR}/vibro_band_rms.c
  ${COMMON_DIR}/vibro_fft.c
  ${COMMON_DIR}/metrics_compact.c
  ${COMMON_DIR}/vibro_features.c
  ${COMMON_DIR}/vibro_verdict_store.c
  ${COMMON_DIR}/vibro_ref_store.c
  ${COMMON_DIR}/vibro_schedule.c
  ${COMMON_DIR}/walk_distance.c
)

target_link_libraries(app PRIVATE m)

# Espressif heap Kconfig → compile defs (heap_caps_zephyr still uses k_malloc;
# SYS_HEAP_HARDENING_* does the real canary work. These defs keep IDF headers
# and any multi_heap paths consistent when present).
if(CONFIG_HEAP_POISONING_LIGHT OR CONFIG_HEAP_POISONING_COMPREHENSIVE)
  zephyr_compile_definitions(MULTI_HEAP_POISONING=1)
endif()
if(CONFIG_HEAP_POISONING_COMPREHENSIVE)
  zephyr_compile_definitions(MULTI_HEAP_POISONING_SLOW=1)
endif()
