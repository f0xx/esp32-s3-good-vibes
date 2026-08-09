/*
 * BLE net GATT — WiFi wizard parity with Arduino ble_net_provider.cpp
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "ble_imu_gatt.h"
#include "ble_net_gatt.h"
#include "ble_net_protocol.h"
#include "net_profile_store.h"
#include "network_manager.h"
#include "radio_scheduler.h"

LOG_MODULE_REGISTER(ble_net, LOG_LEVEL_INF);

static struct bt_uuid_128 net_svc_uuid = BT_UUID_INIT_128(BT_UUID_NET_SVC_VAL);
static struct bt_uuid_128 net_scan_uuid = BT_UUID_INIT_128(BT_UUID_NET_SCAN_VAL);
static struct bt_uuid_128 net_profiles_uuid = BT_UUID_INIT_128(BT_UUID_NET_PROFILES_VAL);
static struct bt_uuid_128 net_cmd_uuid = BT_UUID_INIT_128(BT_UUID_NET_CMD_VAL);
static struct bt_uuid_128 net_status_uuid = BT_UUID_INIT_128(BT_UUID_NET_STATUS_VAL);

static char g_scan_json[BLE_NET_JSON_MAX];
static char g_profiles_json[BLE_NET_JSON_MAX];
static char g_status_json[BLE_NET_JSON_MAX];
static bool g_scan_notify;
static bool g_profiles_notify;
static bool g_status_notify;
static struct bt_gatt_attr *g_scan_attr;
static struct bt_gatt_attr *g_profiles_attr;
static struct bt_gatt_attr *g_status_attr;

static int64_t g_scan_after_ms;

#define NET_PUB_SCAN     BIT(0)
#define NET_PUB_PROFILES BIT(1)
#define NET_PUB_STATUS   BIT(2)

static atomic_t g_defer_pub;

static void publish_scan(void);
static void publish_profiles(void);
static void publish_status(void);

static void net_schedule_publish(uint8_t which)
{
	atomic_or(&g_defer_pub, which);
}

static void net_flush_deferred_publish(void)
{
	if (ble_imu_in_connect_grace()) {
		return;
	}

	const uint8_t pending = (uint8_t)atomic_clear(&g_defer_pub);

	if ((pending & NET_PUB_SCAN) != 0U) {
		publish_scan();
	}
	if ((pending & NET_PUB_PROFILES) != 0U) {
		publish_profiles();
	}
	if ((pending & NET_PUB_STATUS) != 0U) {
		publish_status();
	}
}

static void sync_ble_coex(void)
{
	radio_scheduler_sync();
}

static void refresh_scan(void)
{
	network_manager_build_scan_json(g_scan_json, sizeof(g_scan_json));
}

static void refresh_profiles(void)
{
	network_manager_build_profiles_json(g_profiles_json, sizeof(g_profiles_json));
}

static void refresh_status(const char *state)
{
	network_manager_build_status_json(g_status_json, sizeof(g_status_json), state);
}

static void publish_scan(void)
{
	if (!g_scan_notify || g_scan_attr == NULL) {
		return;
	}

	(void)bt_gatt_notify(NULL, g_scan_attr, g_scan_json, strlen(g_scan_json));
}

static void publish_profiles(void)
{
	if (!g_profiles_notify || g_profiles_attr == NULL) {
		return;
	}

	(void)bt_gatt_notify(NULL, g_profiles_attr, g_profiles_json, strlen(g_profiles_json));
}

static void publish_status(void)
{
	if (!g_status_notify || g_status_attr == NULL) {
		return;
	}

	(void)bt_gatt_notify(NULL, g_status_attr, g_status_json, strlen(g_status_json));
}

static void on_wifi_status(const char *state)
{
	refresh_status(state);
	net_schedule_publish(NET_PUB_STATUS);
	if (state != NULL &&
	    (strcmp(state, "connected") == 0 || strcmp(state, "failed") == 0)) {
		refresh_profiles();
		net_schedule_publish(NET_PUB_PROFILES);
	}
}

static void run_scheduled_scan(void)
{
	if (!network_manager_start_scan()) {
		LOG_WRN("WiFi scan start failed");
		refresh_scan();
		refresh_status("scan_failed");
		sync_ble_coex();
	} else {
		snprintf(g_scan_json, sizeof(g_scan_json), "{\"aps\":[],\"scanning\":1}");
		refresh_status("scanning");
	}

	publish_scan();
	publish_status();
	sync_ble_coex();
}

static bool extract_json_string(const char *json, const char *key, char *out, size_t out_len)
{
	char pattern[32];
	char *start;

	if (json == NULL || key == NULL || out == NULL || out_len == 0) {
		return false;
	}

	snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
	start = strstr(json, pattern);
	if (start == NULL) {
		return false;
	}
	start += strlen(pattern);

	size_t o = 0;
	for (const char *p = start; *p != '\0' && o + 1 < out_len; p++) {
		if (*p == '\\' && *(p + 1) != '\0') {
			out[o++] = *++p;
			continue;
		}
		if (*p == '"') {
			out[o] = '\0';
			return true;
		}
		out[o++] = *p;
	}
	return false;
}

static int extract_json_int(const char *json, const char *key, int default_value)
{
	char pattern[32];
	const char *start;

	snprintf(pattern, sizeof(pattern), "\"%s\":", key);
	start = strstr(json, pattern);
	if (start == NULL) {
		return default_value;
	}
	start += strlen(pattern);
	while (*start == ' ') {
		start++;
	}
	return atoi(start);
}

static ssize_t read_scan(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			 uint16_t len, uint16_t offset)
{
	ARG_UNUSED(attr);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, g_scan_json, strlen(g_scan_json));
}

static ssize_t read_profiles(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			     uint16_t len, uint16_t offset)
{
	ARG_UNUSED(attr);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, g_profiles_json,
				 strlen(g_profiles_json));
}

static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			   uint16_t len, uint16_t offset)
{
	ARG_UNUSED(attr);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, g_status_json,
				 strlen(g_status_json));
}

static void scan_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	g_scan_notify = (value == BT_GATT_CCC_NOTIFY);
	if (g_scan_notify) {
		net_schedule_publish(NET_PUB_SCAN);
	}
}

static void profiles_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	g_profiles_notify = (value == BT_GATT_CCC_NOTIFY);
	if (g_profiles_notify) {
		net_schedule_publish(NET_PUB_PROFILES);
	}
}

static void status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	g_status_notify = (value == BT_GATT_CCC_NOTIFY);
	if (g_status_notify) {
		net_schedule_publish(NET_PUB_STATUS);
	}
}

static ssize_t write_profiles(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			      const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0 || len == 0 || len >= BLE_NET_JSON_MAX) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	char json[BLE_NET_JSON_MAX];

	memcpy(json, buf, len);
	json[len] = '\0';

	if (strstr(json, "\"delete\"") != NULL) {
		const int idx = extract_json_int(json, "idx", -1);

		if (idx >= 0) {
			net_profile_store_remove_index((uint8_t)idx);
		} else {
			char ssid[33];

			if (extract_json_string(json, "ssid", ssid, sizeof(ssid))) {
				net_profile_store_remove_ssid(ssid);
			}
		}
	} else {
		char ssid[33];
		char pass[65];

		if (!extract_json_string(json, "ssid", ssid, sizeof(ssid))) {
			return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
		}
		(void)extract_json_string(json, "pass", pass, sizeof(pass));
		net_profile_store_upsert(ssid, pass, 0);
	}

	refresh_profiles();
	refresh_status("profiles");
	net_schedule_publish(NET_PUB_PROFILES | NET_PUB_STATUS);
	return len;
}

static ssize_t write_cmd(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			 uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0 || len == 0 || len >= BLE_NET_JSON_MAX) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	char json[BLE_NET_JSON_MAX];
	char op[16];

	memcpy(json, buf, len);
	json[len] = '\0';

	if (!extract_json_string(json, "op", op, sizeof(op))) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	if (strcmp(op, "scan") == 0) {
		refresh_status("scanning");
		net_schedule_publish(NET_PUB_STATUS);
		radio_scheduler_set_wifi_busy(true);
		g_scan_after_ms = k_uptime_get() + 150;
		return len;
	}
	if (strcmp(op, "profiles") == 0) {
		refresh_profiles();
		refresh_status("profiles");
		net_schedule_publish(NET_PUB_PROFILES | NET_PUB_STATUS);
		return len;
	}
	if (strcmp(op, "activate") == 0 || strcmp(op, "connect") == 0) {
		const int idx = extract_json_int(json, "idx", -1);
		bool started = false;

		radio_scheduler_set_wifi_busy(true);
		if (idx >= 0) {
			refresh_status("connecting");
			net_schedule_publish(NET_PUB_STATUS);
			started = network_manager_connect_index((uint8_t)idx);
		} else {
			char ssid[33];
			char pass[65];

			if (!extract_json_string(json, "ssid", ssid, sizeof(ssid))) {
				refresh_status("failed");
				net_schedule_publish(NET_PUB_STATUS);
				return len;
			}
			(void)extract_json_string(json, "pass", pass, sizeof(pass));
			refresh_status("connecting");
			net_schedule_publish(NET_PUB_STATUS);
			started = network_manager_connect_creds(ssid, pass);
		}

		if (!started) {
			refresh_status("failed");
			net_schedule_publish(NET_PUB_STATUS);
		}
		return len;
	}
	if (strcmp(op, "prov") == 0) {
		refresh_status("portal");
		net_schedule_publish(NET_PUB_STATUS);
		return len;
	}
	if (strcmp(op, "delete") == 0) {
		const int idx = extract_json_int(json, "idx", -1);

		if (idx >= 0) {
			net_profile_store_remove_index((uint8_t)idx);
		} else {
			char ssid[33];

			if (extract_json_string(json, "ssid", ssid, sizeof(ssid))) {
				net_profile_store_remove_ssid(ssid);
			}
		}
		refresh_profiles();
		refresh_status("profiles");
		net_schedule_publish(NET_PUB_PROFILES | NET_PUB_STATUS);
	}

	return len;
}

BT_GATT_SERVICE_DEFINE(
	net_svc, BT_GATT_PRIMARY_SERVICE(&net_svc_uuid),
	BT_GATT_CHARACTERISTIC(&net_scan_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_scan, NULL, NULL),
	BT_GATT_CCC(scan_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&net_profiles_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, read_profiles, write_profiles,
			       NULL),
	BT_GATT_CCC(profiles_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&net_cmd_uuid.uuid, BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE, NULL,
			       write_cmd, NULL),
	BT_GATT_CHARACTERISTIC(&net_status_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_status, NULL, NULL),
	BT_GATT_CCC(status_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

int ble_net_gatt_init(void)
{
	g_scan_attr = bt_gatt_find_by_uuid(net_svc.attrs, net_svc.attr_count, &net_scan_uuid.uuid);
	g_profiles_attr =
		bt_gatt_find_by_uuid(net_svc.attrs, net_svc.attr_count, &net_profiles_uuid.uuid);
	g_status_attr =
		bt_gatt_find_by_uuid(net_svc.attrs, net_svc.attr_count, &net_status_uuid.uuid);

	network_manager_set_wifi_status_cb(on_wifi_status);

	refresh_scan();
	refresh_profiles();
	refresh_status("idle");
	LOG_INF("BLE net service registered");
	return 0;
}

void ble_net_gatt_tick(void)
{
	static bool last_scanning;

	if (g_scan_after_ms > 0 && k_uptime_get() >= g_scan_after_ms) {
		g_scan_after_ms = 0;
		run_scheduled_scan();
	}

	net_flush_deferred_publish();

	const bool scanning = network_manager_scan_busy();

	if (last_scanning && !scanning) {
		refresh_scan();
		refresh_status("scan_done");
		publish_scan();
		publish_status();
	}
	last_scanning = scanning;
	sync_ble_coex();
	net_flush_deferred_publish();
}
