#include "ble_crash_gatt.h"
#include "ble_imu_gatt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "crash_report.h"
#include "crash_debug.h"
#include "bist.h"
#include "crash_ring_store.h"

LOG_MODULE_REGISTER(ble_crash, LOG_LEVEL_INF);

#define BT_UUID_CRASH_SVC_VAL \
	BT_UUID_128_ENCODE(0x4a6e0301, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)
#define BT_UUID_CRASH_INFO_VAL \
	BT_UUID_128_ENCODE(0x4a6e0302, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)
#define BT_UUID_CRASH_CTRL_VAL \
	BT_UUID_128_ENCODE(0x4a6e0303, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)
#define BT_UUID_CRASH_DATA_VAL \
	BT_UUID_128_ENCODE(0x4a6e0304, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)

static struct bt_uuid_128 crash_svc_uuid = BT_UUID_INIT_128(BT_UUID_CRASH_SVC_VAL);
static struct bt_uuid_128 crash_info_uuid = BT_UUID_INIT_128(BT_UUID_CRASH_INFO_VAL);
static struct bt_uuid_128 crash_ctrl_uuid = BT_UUID_INIT_128(BT_UUID_CRASH_CTRL_VAL);
static struct bt_uuid_128 crash_data_uuid = BT_UUID_INIT_128(BT_UUID_CRASH_DATA_VAL);

/* BLE ATT attribute values are hard-capped at 512B by the Bluetooth spec (BT_ATT_MAX_ATTRIBUTE_LEN)
 * regardless of buffer size here, so this must stay <= that. crash_ring_list_json() keeps each
 * bulk-mode record compact (~190-210B) so 2 pending crashes usually fit in one read; any
 * remainder is picked up on the relay's next round instead of overflowing. */
static char g_info_json[500];
static uint8_t g_data_buf[512];
static size_t g_data_len;
static off_t g_data_off;
static char g_ctrl_json[128];
static atomic_t g_defer_ctrl;

static void refresh_info_json(void)
{
	const int n = crash_report_info_json(g_info_json, sizeof(g_info_json));

	if (n <= 0) {
		snprintf(g_info_json, sizeof(g_info_json), "{\"pending\":0}");
	}
}

static bool parse_read_cmd(const char *json, off_t *off, size_t *len)
{
	const char *key_off = strstr(json, "\"off\"");
	const char *key_len = strstr(json, "\"len\"");

	if (key_off == NULL || key_len == NULL) {
		return false;
	}

	key_off = strchr(key_off, ':');
	key_len = strchr(key_len, ':');
	if (key_off == NULL || key_len == NULL) {
		return false;
	}

	*off = (off_t)strtoul(key_off + 1, NULL, 10);
	*len = (size_t)strtoul(key_len + 1, NULL, 10);
	if (*len == 0U) {
		*len = sizeof(g_data_buf);
	}
	if (*len > sizeof(g_data_buf)) {
		*len = sizeof(g_data_buf);
	}
	return true;
}

static bool parse_slot_cmd(const char *json, int *slot_out)
{
	const char *key = strstr(json, "\"slot\"");

	if (key == NULL || slot_out == NULL) {
		return false;
	}

	key = strchr(key, ':');
	if (key == NULL) {
		return false;
	}

	*slot_out = (int)strtoul(key + 1, NULL, 10);
	return *slot_out >= 0 && *slot_out < (int)CRASH_RING_SLOTS;
}

/* Parses {"op":"clear","slots":[0,2,3]} — lets the phone clear every slot it just confirmed
 * uploaded to the cloud in one BLE write instead of one write per slot. Returns the number of
 * slots parsed (0 if the key is absent so the caller falls back to single-slot/clear-all). */
static int parse_slots_array(const char *json, uint8_t *slots_out, int max_slots)
{
	const char *key = strstr(json, "\"slots\"");
	int count = 0;

	if (key == NULL) {
		return 0;
	}
	key = strchr(key, '[');
	if (key == NULL) {
		return 0;
	}
	key++;

	while (*key != '\0' && *key != ']' && count < max_slots) {
		while (*key == ' ' || *key == ',') {
			key++;
		}
		if (*key == ']' || *key == '\0') {
			break;
		}
		char *end = NULL;
		long v = strtol(key, &end, 10);

		if (end == key) {
			break;
		}
		if (v >= 0 && v < (long)CRASH_RING_SLOTS) {
			slots_out[count++] = (uint8_t)v;
		}
		key = end;
	}
	return count;
}

static ssize_t read_info(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			 uint16_t len, uint16_t offset)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, g_info_json, strlen(g_info_json));
}

static ssize_t read_data(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			 uint16_t len, uint16_t offset)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, g_data_buf, g_data_len);
}

static ssize_t write_ctrl(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			  uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0 || len == 0 || len >= sizeof(g_ctrl_json)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	memcpy(g_ctrl_json, buf, len);
	g_ctrl_json[len] = '\0';
	atomic_set(&g_defer_ctrl, 1);
	return len;
}

static void process_ctrl_json(void)
{
	char *json = g_ctrl_json;

	if (strstr(json, "\"op\":\"clear\"") != NULL) {
		uint8_t slots[CRASH_RING_SLOTS];
		int n = parse_slots_array(json, slots, (int)ARRAY_SIZE(slots));
		int slot = -1;

		if (n > 0) {
			/* Single erase+rewrite for all N slots — see crash_report_clear_slots()'s
			 * doc comment for why looping crash_report_clear_slot() per slot here used
			 * to chain N full flash-sector erases inside one BLE write callback, which
			 * was long enough to starve the main-loop stall watchdog and reboot the
			 * device mid-clear (surfaced to the phone as "crash clear failed" +
			 * a dropped link). */
			crash_report_clear_slots(slots, (size_t)n);
			LOG_INF("crash slots cleared via BLE (n=%d)", n);
		} else if (parse_slot_cmd(json, &slot)) {
			crash_report_clear_slot((uint8_t)slot);
			LOG_INF("crash slot %d cleared via BLE", slot);
		} else {
			crash_report_clear();
			LOG_INF("crash ring cleared via BLE");
		}
		g_data_len = 0;
		refresh_info_json();
		return;
	}

	if (strstr(json, "\"op\":\"info\"") != NULL) {
		int slot = -1;

		if (!parse_slot_cmd(json, &slot)) {
			return;
		}
		(void)crash_report_info_json_slot((uint8_t)slot, g_info_json, sizeof(g_info_json));
		return;
	}

	if (strstr(json, "\"op\":\"list\"") != NULL) {
		(void)crash_report_list_json(g_info_json, sizeof(g_info_json));
		return;
	}

	if (strstr(json, "\"op\":\"read\"") != NULL) {
		off_t off = 0;
		size_t read_len = sizeof(g_data_buf);

		if (!parse_read_cmd(json, &off, &read_len)) {
			return;
		}

		const int n = crash_report_dump_read(off, g_data_buf, read_len);

		if (n < 0) {
			g_data_len = 0;
			return;
		}

		g_data_off = off;
		g_data_len = (size_t)n;
		LOG_DBG("crash dump read off=%ld len=%u", (long)off, (unsigned)g_data_len);
		return;
	}

#if defined(CONFIG_APP_CRASH_DEBUG)
	if (strstr(json, "\"op\":\"inject\"") != NULL) {
		const char *key = strstr(json, "\"kind\"");
		const char *kind = "panic";

		if (key != NULL) {
			const char *colon = strchr(key, ':');

			if (colon != NULL) {
				colon++;
				while (*colon == ' ' || *colon == '\t') {
					colon++;
				}
				if (*colon == '"') {
					colon++;
					kind = colon;
				}
			}
		}
		char kind_buf[16];
		size_t ki = 0;

		while (kind[ki] != '\0' && kind[ki] != '"' && ki + 1U < sizeof(kind_buf)) {
			kind_buf[ki] = kind[ki];
			ki++;
		}
		kind_buf[ki] = '\0';
		if (!crash_debug_schedule_inject(kind_buf[0] != '\0' ? kind_buf : "panic")) {
			return;
		}
		LOG_WRN("crash inject scheduled: %s", kind_buf);
		return;
	}

	if (strstr(json, "\"op\":\"bist\"") != NULL) {
		bist_run();
		refresh_info_json();
		return;
	}
#endif
}

BT_GATT_SERVICE_DEFINE(
	crash_svc, BT_GATT_PRIMARY_SERVICE(&crash_svc_uuid),
	BT_GATT_CHARACTERISTIC(&crash_info_uuid.uuid, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, read_info, NULL, NULL),
	BT_GATT_CHARACTERISTIC(&crash_ctrl_uuid.uuid, BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE, NULL, write_ctrl, NULL),
	BT_GATT_CHARACTERISTIC(&crash_data_uuid.uuid, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, read_data, NULL, NULL), );

int ble_crash_gatt_init(void)
{
	refresh_info_json();
	LOG_INF("BLE crash service (pending=%u)", crash_report_pending_count());
	return 0;
}

bool ble_crash_gatt_pending(void)
{
	return crash_report_pending();
}

void ble_crash_gatt_looper_tick(void)
{
	if (!atomic_get(&g_defer_ctrl)) {
		return;
	}

	/* Dev inject/BIST must run immediately — connect grace (12s) blocked user-triggered
	 * faults and made the mobile "Crash debug" menu appear dead. "clear" is also urgent:
	 * running it exactly at the grace-elapse tick (the same tick that kicks off batch-prep
	 * once notifications are enabled, see ble_imu_gatt.c's g_grace_prep_pending) seemed to
	 * correlate with the intermittent flash-erase/BLE TG0WDT_SYS_RST reset (see
	 * persist_ring()'s doc comment) — running the clear's flash write earlier, decoupled
	 * from that boundary, avoids stacking it against whatever else fires there. Read dump
	 * still defers until grace ends. */
	const bool urgent = strstr(g_ctrl_json, "\"op\":\"inject\"") != NULL ||
			    strstr(g_ctrl_json, "\"op\":\"bist\"") != NULL ||
			    strstr(g_ctrl_json, "\"op\":\"clear\"") != NULL;

	if (!urgent && ble_imu_in_connect_grace()) {
		return;
	}

	if (atomic_cas(&g_defer_ctrl, 1, 0)) {
		process_ctrl_json();
	}
}
