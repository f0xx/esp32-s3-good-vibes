#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

#include "device_config.h"
#include "board_config.h"

struct crash_ring_telemetry;

void power_manager_set_display(const struct device *display);
void power_manager_set_imu_reschedule_cb(void (*cb)(void));
void power_manager_set_ble_active(bool active);
/** BT controller enabled (bt_enable) — keep 240 MHz until off; downclock hangs after init. */
void power_manager_set_bt_controller_on(bool on);
void power_manager_set_panel_hw_fn(bool (*fn)(bool on));
void power_manager_set_display_busy_query(bool (*fn)(void));
void power_manager_init(void);
void power_manager_mark_ready(void);
void power_manager_tick(void);
void power_manager_on_screen(bool on);
bool power_manager_screen_on(void);
/** Demo (false) / staging (true) — cycled by BOOT tap; drives CPU/render/IMU tiers below. */
bool power_manager_staging_mode(void);
/** BOOT tap handler: flips demo<->staging, persists, re-applies targets immediately. */
void power_manager_toggle_mode(void);
bool power_manager_tft_render_enabled(void);
/** Queue panel blanking/backlight change (applied on render thread). */
void power_manager_request_panel_hw(bool on);
/** Panel blanking/backlight (render thread only). */
void power_manager_sync_render(void);
/** Apply deferred panel blanking/backlight (render thread only). */
void power_manager_sync_panel_hw(void);
/** Re-apply backlight PWM after radio init; no-op when screen is off. */
void power_manager_reapply_backlight_if_screen_on(void);
uint32_t power_manager_imu_interval_ms(void);
uint32_t power_manager_render_interval_ms(void);

/** Last value passed to clock_control_configure (may lag actual PLL). */
uint8_t power_manager_cpu_mhz_settled(void);
/** Target from current screen/profile policy. */
uint8_t power_manager_cpu_mhz_target(void);
/** Measured from SoC clock tree (Hz/1e6). */
uint8_t power_manager_cpu_mhz_actual(void);
uint8_t power_manager_apb_mhz_actual(void);
uint8_t power_manager_render_hz_target(void);
uint8_t power_manager_imu_hz_target(void);

void power_manager_log_telemetry(uint32_t render_frames, uint32_t imu_ticks,
				 uint32_t window_ms, uint32_t flush_ms);

void power_manager_telemetry_snapshot(struct crash_ring_telemetry *out);
