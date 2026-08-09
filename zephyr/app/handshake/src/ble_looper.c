/*
 * ble_looper — FIFO msgq owned and drained by the main thread only.
 * Avoids sysworkq stack overflow (build_batch + do_swap corruption at ~50s).
 */

#include "ble_looper.h"

#include "ble_crash_gatt.h"
#include "ble_imu_gatt.h"

#include <zephyr/bluetooth/conn.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ble_looper, LOG_LEVEL_INF);

#define BLE_LOOPER_Q_DEPTH 20

enum ble_looper_evt {
	BLE_LOOPER_CONNECTED = 1,
	BLE_LOOPER_DISCONNECTED,
	BLE_LOOPER_ADV_START,
	BLE_LOOPER_ADV_RESTART,
};

struct ble_looper_msg {
	uint8_t type;
	uint8_t u8;
	struct bt_conn *conn;
};

K_MSGQ_DEFINE(ble_looper_q, sizeof(struct ble_looper_msg), BLE_LOOPER_Q_DEPTH, 4);

static void dispatch_msg(const struct ble_looper_msg *msg)
{
	switch (msg->type) {
	case BLE_LOOPER_CONNECTED:
		ble_imu_gatt_looper_connected(msg->conn);
		break;
	case BLE_LOOPER_DISCONNECTED:
		ble_imu_gatt_looper_disconnected(msg->u8);
		break;
	case BLE_LOOPER_ADV_START:
		(void)ble_imu_gatt_looper_adv_start(false);
		break;
	case BLE_LOOPER_ADV_RESTART:
		(void)ble_imu_gatt_looper_adv_start(true);
		break;
	default:
		LOG_WRN("ble_looper: unknown evt %u", msg->type);
		break;
	}
}

static int ble_looper_post(const struct ble_looper_msg *msg)
{
	if (k_msgq_put(&ble_looper_q, msg, K_NO_WAIT) != 0) {
		LOG_WRN("ble_looper: queue full (drop evt %u)", msg->type);
		if (msg->type == BLE_LOOPER_CONNECTED && msg->conn != NULL) {
			bt_conn_unref(msg->conn);
		}
		return -ENOMEM;
	}

	return 0;
}

int ble_looper_init(void)
{
	LOG_INF("ble_looper ready (main-thread FIFO depth=%u)", BLE_LOOPER_Q_DEPTH);
	return 0;
}

void ble_looper_poll(void)
{
	for (;;) {
		struct ble_looper_msg msg;

		if (k_msgq_get(&ble_looper_q, &msg, K_NO_WAIT) != 0) {
			break;
		}

		dispatch_msg(&msg);

		if (msg.type == BLE_LOOPER_CONNECTED && msg.conn != NULL) {
			bt_conn_unref(msg.conn);
		}
	}

	ble_imu_gatt_looper_tick();
	ble_crash_gatt_looper_tick();
}

int ble_looper_post_connected(struct bt_conn *conn, uint8_t err)
{
	struct ble_looper_msg msg = {
		.type = BLE_LOOPER_CONNECTED,
		.u8 = err,
		.conn = NULL,
	};

	if (err != 0U || conn == NULL) {
		LOG_ERR("connect failed (%u)", err);
		return 0;
	}

	msg.conn = bt_conn_ref(conn);
	return ble_looper_post(&msg);
}

int ble_looper_post_disconnected(uint8_t reason)
{
	const struct ble_looper_msg msg = {
		.type = BLE_LOOPER_DISCONNECTED,
		.u8 = reason,
	};

	return ble_looper_post(&msg);
}

int ble_looper_post_adv_start(void)
{
	const struct ble_looper_msg msg = { .type = BLE_LOOPER_ADV_START };

	return ble_looper_post(&msg);
}

int ble_looper_post_adv_restart(void)
{
	const struct ble_looper_msg msg = { .type = BLE_LOOPER_ADV_RESTART };

	return ble_looper_post(&msg);
}
