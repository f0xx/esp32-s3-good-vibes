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

#define BLE_IMU_HEADER_RESERVE    96
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

enum ble_imu_mode {
	BLE_IMU_MODE_RAW = 0,
	BLE_IMU_MODE_COMPUTED = 1,
	BLE_IMU_MODE_SCENE = 2,
};

/* Capability bitmask — device_caps.h / ImuProtocol.kt */
enum ble_imu_cap {
	BLE_CAP_IMU = 1u << 0,
	BLE_CAP_TFT = 1u << 1,
	BLE_CAP_BLE_CONFIG = 1u << 2,
	BLE_CAP_CHIP_TEMP = 1u << 3,
	BLE_CAP_VIBRO = 1u << 4,
	BLE_CAP_WIFI = 1u << 5,
	BLE_CAP_OTA = 1u << 6,
	BLE_CAP_CRASH_DEBUG = 1u << 7,
};

static inline uint32_t ble_imu_zephyr_caps(void)
{
	uint32_t caps = BLE_CAP_IMU | BLE_CAP_TFT | BLE_CAP_BLE_CONFIG | BLE_CAP_CHIP_TEMP |
			BLE_CAP_VIBRO | BLE_CAP_OTA;

#if IS_ENABLED(CONFIG_WIFI)
	caps |= BLE_CAP_WIFI;
#endif
#if defined(CONFIG_APP_CRASH_DEBUG)
	caps |= BLE_CAP_CRASH_DEBUG;
#endif
	return caps;
}
