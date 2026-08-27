/*
 * Shared BLE IMU protocol — keep in sync with:
 *   esp32_s3_imu_basics/ble/ble_protocol.h
 *   android/.../ImuProtocol.kt
 */

#pragma once

#include <stdint.h>

#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>

#define BLE_IMU_DEVICE_NAME "ESP32S3 IMU sim"

#define BLE_IMU_DEFAULT_POLL_MS 33
#define BLE_IMU_POLL_MS_MIN     33
#define BLE_IMU_POLL_MS_MAX     2000

#define BLE_IMU_ATT_PAYLOAD_MAX 512
/** STATUS is read-only (long-read); separate from NOTIFY batch cap. */
#define BLE_IMU_STATUS_JSON_MAX 800

#define BLE_IMU_HEADER_RESERVE    160
#define BLE_IMU_COMMIT_BYTES      (BLE_IMU_ATT_PAYLOAD_MAX - BLE_IMU_HEADER_RESERVE)

/* 128-bit UUID base (Zephyr BT_UUID_128_ENCODE format) */
#define BT_UUID_IMU_SVC_VAL \
	BT_UUID_128_ENCODE(0x4a6e0001, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)
#define BT_UUID_IMU_MODE_VAL \
	BT_UUID_128_ENCODE(0x4a6e0002, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)
#define BT_UUID_IMU_STATUS_VAL \
	BT_UUID_128_ENCODE(0x4a6e0003, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)
#define BT_UUID_IMU_DATA_VAL \
	BT_UUID_128_ENCODE(0x4a6e0004, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)
#define BT_UUID_IMU_POLL_MS_VAL \
	BT_UUID_128_ENCODE(0x4a6e0005, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)
#define BT_UUID_IMU_NOTIFY_VAL \
	BT_UUID_128_ENCODE(0x4a6e0006, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)
#define BT_UUID_IMU_TIME_VAL \
	BT_UUID_128_ENCODE(0x4a6e0007, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)
#define BT_UUID_IMU_CAPS_VAL \
	BT_UUID_128_ENCODE(0x4a6e0008, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)
#define BT_UUID_IMU_SCREEN_VAL \
	BT_UUID_128_ENCODE(0x4a6e0009, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)
/** Manual CPU clock override, 1 byte: 0 = auto (mode-derived), else explicit target MHz
 *  (rounded to the nearest supported tier — see power_manager.c clamp_cpu_mhz()). */
#define BT_UUID_IMU_CPU_MHZ_VAL \
	BT_UUID_128_ENCODE(0x4a6e000a, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)
/** Manual IMU sample-rate override, 1 byte: 0 = auto (mode-derived), else explicit Hz
 *  (clamped to [BLE_IMU_HZ_OVERRIDE_MIN, BLE_IMU_HZ_OVERRIDE_MAX]). */
#define BT_UUID_IMU_IMU_HZ_VAL \
	BT_UUID_128_ENCODE(0x4a6e000b, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)
/** Battery bench control: write 1 byte — 0=STOP, 1=START. Read 9 bytes — active(1),
 *  session_id(u32 LE), sample_seq(u32 LE). Config locked while active. */
#define BT_UUID_IMU_BENCH_VAL \
	BT_UUID_128_ENCODE(0x4a6e000c, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)

#define BLE_IMU_BENCH_CMD_STOP  0U
#define BLE_IMU_BENCH_CMD_START 1U

#define BLE_IMU_HZ_OVERRIDE_MIN 1
#define BLE_IMU_HZ_OVERRIDE_MAX 120

enum ble_imu_mode {
	BLE_IMU_MODE_RAW = 0,
	BLE_IMU_MODE_COMPUTED = 1,
	BLE_IMU_MODE_SCENE = 2,
};

/* Capability bitmask — 4 bytes LE on CHAR_CAPS (4a6e0008). caps==0 is the
 * legacy/unknown case. Do not reshuffle bits 0–7; extra bytes on a later
 * 8/16-byte read are ignored by current phones (first 4 still win).
 *
 *  0 IMU            1 TFT           2 BLE_CONFIG     3 CHIP_TEMP
 *  4 VIBRO          5 WIFI          6 OTA            7 CRASH_DEBUG
 *  8 MT200 bridge   9 RSSI wrssi
 * 10–31 reserved (TIME/BENCH/SCENE/compact are UUID-discoverable) */
enum ble_imu_cap {
	BLE_CAP_IMU = 1u << 0,
	BLE_CAP_TFT = 1u << 1,
	BLE_CAP_BLE_CONFIG = 1u << 2,
	BLE_CAP_CHIP_TEMP = 1u << 3,
	BLE_CAP_VIBRO = 1u << 4,
	BLE_CAP_WIFI = 1u << 5,
	BLE_CAP_OTA = 1u << 6,
	BLE_CAP_CRASH_DEBUG = 1u << 7,
	BLE_CAP_MT200 = 1u << 8,
	BLE_CAP_RSSI = 1u << 9,
};

static inline uint32_t ble_imu_zephyr_caps(void)
{
	uint32_t caps = BLE_CAP_IMU | BLE_CAP_TFT | BLE_CAP_BLE_CONFIG | BLE_CAP_CHIP_TEMP |
			BLE_CAP_VIBRO | BLE_CAP_OTA | BLE_CAP_RSSI;

#if IS_ENABLED(CONFIG_WIFI)
	caps |= BLE_CAP_WIFI;
#endif
#if defined(CONFIG_APP_CRASH_DEBUG)
	caps |= BLE_CAP_CRASH_DEBUG | BLE_CAP_MT200;
#endif
	return caps;
}
