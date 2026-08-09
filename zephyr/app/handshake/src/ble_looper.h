#pragma once

#include <stdint.h>

#include <zephyr/bluetooth/conn.h>

/** Main-thread FIFO looper: BT callbacks enqueue, main drains in ble_looper_poll(). */
int ble_looper_init(void);

/** Drain pending BLE events then run GATT tick — call from main loop only. */
void ble_looper_poll(void);

int ble_looper_post_connected(struct bt_conn *conn, uint8_t err);
int ble_looper_post_disconnected(uint8_t reason);
int ble_looper_post_adv_start(void);
int ble_looper_post_adv_restart(void);
