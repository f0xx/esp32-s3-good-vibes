/*
 * Host stub latencies. Flush timing from handshake v322 boot
 * (copy=3ms write=70ms total=73ms). Older 2026-09-15 means are in comments.
 * Disable with HOST_NO_LATENCY=1.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* QMI8658 probe begin→ready: mean 174ms / med 216ms (settle+retries+discard). */
#define HOST_LAT_QMI_SETTLE_MS       30
#define HOST_LAT_QMI_I2C_PROBE_US    800
#define HOST_LAT_QMI_DISCARD_MS      80
#define HOST_LAT_QMI_READ_US         200

/* v322 boot: display_write write=70ms copy=3ms total=73ms (was ~51ms at 20 MHz). */
#define HOST_LAT_RENDER_FLUSH_US     73000
#define HOST_LAT_RENDER_REMAINDER_MS 5

/* ble_imu consecutive short gaps mean 68ms; bt_hci ~1.3ms; conn→NOTIFY med 1413ms. */
#define HOST_LAT_BLE_CALL_US         1300
#define HOST_LAT_BLE_CONN_US         68000
#define HOST_LAT_BLE_NOTIFY_US       87000
#define HOST_LAT_BLE_DRAIN_US        2000
#define HOST_LAT_BLE_SCRIPT_CONN_MS  1500
#define HOST_LAT_BLE_SCRIPT_NOTIFY_MS 2900 /* ~conn + 1413 */

/* net_mgr / ota_wifi adjacent lines mean ~5ms (cap small). */
#define HOST_LAT_WIFI_CALL_US        5000

/* mt200 consecutive lines med 4509ms — too large per call; use a slice. */
#define HOST_LAT_MT200_CALL_US       4500

void host_latency_init(void);
bool host_latency_enabled(void);

/** Sleep (or virtual-time advance) for calibrated stub cost. */
void host_latency_us(uint32_t us);
void host_latency_ms(uint32_t ms);

/**
 * Panel flush: hold renderer_busy + the ext-bus mutex for HOST_LAT_RENDER_FLUSH_US.
 * IMU I2C try-lock fails for that window, same as the desk SPI/I2C gate.
 */
void host_panel_flush(void);
