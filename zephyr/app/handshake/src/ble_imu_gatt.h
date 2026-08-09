#pragma once

#include <zephyr/bluetooth/conn.h>

int ble_imu_gatt_init(void);
void ble_imu_gatt_set_traffic_paused(bool paused);
bool ble_imu_link_active(void);
bool ble_imu_in_connect_grace(void);

/** Called only from ble_looper thread. */
void ble_imu_gatt_looper_tick(void);
void ble_imu_gatt_looper_connected(struct bt_conn *conn);
void ble_imu_gatt_looper_disconnected(uint8_t reason);
int ble_imu_gatt_looper_adv_start(bool restart);

/** bt_conn callbacks — enqueue to ble_looper only. */
void ble_imu_on_connected(struct bt_conn *conn, uint8_t err);
void ble_imu_on_disconnected(struct bt_conn *conn, uint8_t reason);
