/*
 * DeviceConfigV1 — binary blob parity with esp32 config/device_config.h / Android ConfigProtocol
 */

#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"

#define DEVICE_CONFIG_MAGIC    0x31494D55U /* "IMU1" LE */
#define DEVICE_CONFIG_VERSION  1U
#define DEVICE_CONFIG_BLOB_MAX 512U

#pragma pack(push, 1)
struct device_config_v1 {
	uint32_t magic;
	uint16_t version;
	uint16_t size;
	uint32_t crc32;

	float bat_full_v;
	float bat_empty_v;
	float bat_offset;
	float bat_dc_margin_v;
	float bat_trend_rise_v;
	float bat_trend_fall_v;
	float bat_trend_stable_v;
	uint8_t bat_trend_window;
	uint8_t bat_trend_compare;
	uint8_t bat_dc_confirm;
	uint8_t bat_bat_confirm;
	float bat_voltage_ema;
	float bat_percent_ema;

	float zoom_default[3];
	float zoom_min;
	float zoom_max;
	float zoom_motion_min;
	float zoom_step;
	float zoom_smooth;
	uint8_t zoom_accel_mode;
	uint8_t _pad_zoom[3];

	float gyro_off[3];
	float accel_off[3];
	uint8_t gyro_done;
	uint8_t accel_done;
	uint8_t _pad_imu[2];

	float walk_height_m;
	float walk_pocket_m;
	float walk_step_min_m;
	float walk_step_max_m;
	uint8_t walk_surface;
	uint8_t _pad_walk[3];

	uint16_t ble_poll_ms;
	uint8_t ble_default_mode;
	uint8_t _pad_ble;

	float imu_accel_scale;
	float imu_gyro_scale;

	uint8_t power_profile;
	uint8_t tft_policy;
	uint16_t wake_interval_sec;
	uint8_t active_window_sec;
	uint8_t cpu_mhz;
	uint8_t imu_sample_hz;
	uint8_t auto_dc_profile;
	uint8_t deep_sleep_enable;
	uint32_t profile_created_unix;
	uint32_t profile_updated_unix;

	uint8_t vibro_schedule_mode;
	uint8_t vibro_interval_sec;
	uint8_t vibro_window_sec;
	uint8_t vibro_jitter_sec;
	uint8_t vibro_capture_tier;
	uint8_t wifi_upload_enable;
	uint8_t reserved[17]; /*
			       * [0]=mix_every [1]=mix_ratio [2]=dyn_short [3]=dyn_nested
			       * [4..7]=local_revision u32 LE (bit31 = ESP-local namespace)
			       * [8]=local_flags (bit0 = TFT user-off persists across reboot)
			       * [9]=cpu_mhz_override (0=auto/mode-derived, else explicit MHz —
			       *     see power_manager.c apply_rates())
			       * [10]=imu_hz_override (0=auto/mode-derived, else explicit Hz)
			       * Both are ESP-local like [8] (excluded from cloud/remote merge —
			       * see device_config_apply_remote()) since they're a phone-side
			       * manual override of *this device's* live behavior, not profile
			       * data meant to sync.
			       */
};
#pragma pack(pop)

#define DEVICE_CONFIG_LOCAL_REV_FLAG 0x80000000U
#define DEVICE_CONFIG_LOCAL_TFT_OFF  0x01U
/** BOOT-tap cycled demo/staging operating mode (power_manager CPU/render/IMU tiers). */
#define DEVICE_CONFIG_LOCAL_STAGING_MODE 0x02U

enum device_config_apply_result {
	DEVICE_CONFIG_APPLY_OK = 0,
	DEVICE_CONFIG_APPLY_STALE = 1,
	DEVICE_CONFIG_APPLY_INVALID = 2,
};

void device_config_defaults(struct device_config_v1 *cfg);
void device_config_init(void);
void device_config_poll(void);
bool device_config_load(struct device_config_v1 *cfg);
bool device_config_save(const struct device_config_v1 *cfg);
/** Immediate NVS write (boot/erase paths — not from BLE GATT). */
bool device_config_save_sync(const struct device_config_v1 *cfg);
/** Erase Zephyr settings storage partition (fixes corrupt NVS). */
bool device_config_storage_erase(void);
bool device_config_apply_blob(const uint8_t *blob, size_t len);
enum device_config_apply_result device_config_apply_remote(const struct device_config_v1 *incoming);
bool device_config_reload(struct device_config_v1 *cfg);
const struct device_config_v1 *device_config_runtime(void);

uint32_t device_config_cloud_revision(const struct device_config_v1 *cfg);
uint32_t device_config_local_revision(const struct device_config_v1 *cfg);
uint8_t device_config_local_flags(const struct device_config_v1 *cfg);
void device_config_set_user_screen(bool on);
bool device_config_user_screen_off(void);
/** Demo (false) / staging (true) operating mode — persists across reboots, local only. */
bool device_config_staging_mode(void);
void device_config_set_staging_mode(bool staging);
/** Manual CPU clock override (0 = auto/mode-derived), local only, persists across reboots. */
uint8_t device_config_cpu_mhz_override(void);
void device_config_set_cpu_mhz_override(uint8_t mhz);
/** Manual IMU sample-rate override (0 = auto/mode-derived), local only, persists across reboots. */
uint8_t device_config_imu_hz_override(void);
void device_config_set_imu_hz_override(uint8_t hz);
