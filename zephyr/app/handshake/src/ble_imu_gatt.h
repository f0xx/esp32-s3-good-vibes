#pragma once

#include <zephyr/bluetooth/conn.h>

/** Post-connect quiet window — phone should defer MODE/TIME/poll/crash/offload until this
 *  elapses (see ble_imu_gatt.c ble_traffic_ready()). Android ImuProtocol.ESP_CONNECT_SETTLE_MS. */
#define BLE_IMU_CONNECT_GRACE_MS 2000
#define BLE_IMU_POST_GRACE_MS    500

int ble_imu_gatt_init(void);
void ble_imu_gatt_set_traffic_paused(bool paused);
/** True when there is no phone link, or its interval is already ≥200 ms (WiFi can scan). */
bool ble_imu_phone_conn_quiet_for_wifi(void);
/** Re-issue the stretched phone LE interval while waiting to start a WiFi scan. */
void ble_imu_phone_stretch_for_wifi(void);
/** Hold advertising so a WiFi scan can own the 2.4 GHz radio with no BLE traffic. */
void ble_imu_set_hold_adv(bool hold);
/** Hold advertising only while an MT200 central session is live (scan/connect/fetch). */
void ble_imu_set_mt200_adv_hold(bool hold);
/** Drop the phone link after the scan GATT write has completed. */
void ble_imu_disconnect_phone_for_wifi(void);
bool ble_imu_link_active(void);
bool ble_imu_in_connect_grace(void);

/** True once the link has been up for at least (grace + post-grace + extra_margin_ms).
 *  Originally used to keep flash-erase-heavy BLE control ops (e.g. crash-ring clear) away from
 *  the connection-establishment window — turned out a live connection is unsafe for that at
 *  ANY offset, not just near that boundary (see ble_imu_disconnected_settled() and
 *  crash_ring_flush_ram_clears() for the actual fix). Still used for other deferred ops that
 *  only need to avoid the raw connect/grace-elapse windows. Returns false while disconnected. */
bool ble_imu_connect_settled(uint32_t extra_margin_ms);

/** True once there has been NO BLE connection at all for at least min_idle_ms — i.e. the link
 *  layer has fully torn down. See crash_ring_flush_ram_clears()'s doc comment for why a flash
 *  erase needs this instead of merely being "far enough" into a live connection. */
bool ble_imu_disconnected_settled(uint32_t min_idle_ms);

/** Called only from ble_looper thread. */
void ble_imu_gatt_looper_tick(void);
void ble_imu_gatt_looper_connected(struct bt_conn *conn);
void ble_imu_gatt_looper_disconnected(uint8_t reason);
int ble_imu_gatt_looper_adv_start(bool restart);

/** Push STATUS after a battery-bench sample tick (main loop only). */
void ble_imu_gatt_bench_sample(void);

/** bt_conn callbacks — enqueue to ble_looper only. */
void ble_imu_on_connected(struct bt_conn *conn, uint8_t err);
void ble_imu_on_disconnected(struct bt_conn *conn, uint8_t reason);
