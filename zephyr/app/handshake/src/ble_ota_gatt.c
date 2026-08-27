#include "ble_ota_gatt.h"

#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include "ota_ab.h"
#include "soft_reboot.h"

LOG_MODULE_REGISTER(ble_ota, LOG_LEVEL_INF);

#define BT_UUID_OTA_SVC_VAL \
	BT_UUID_128_ENCODE(0x4a6e0201, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)
#define BT_UUID_OTA_CTRL_VAL \
	BT_UUID_128_ENCODE(0x4a6e0202, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)
#define BT_UUID_OTA_DATA_VAL \
	BT_UUID_128_ENCODE(0x4a6e0203, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)

static struct bt_uuid_128 ota_svc_uuid = BT_UUID_INIT_128(BT_UUID_OTA_SVC_VAL);
static struct bt_uuid_128 ota_ctrl_uuid = BT_UUID_INIT_128(BT_UUID_OTA_CTRL_VAL);
static struct bt_uuid_128 ota_data_uuid = BT_UUID_INIT_128(BT_UUID_OTA_DATA_VAL);

static struct flash_img_context g_flash_ctx;
static char g_status[128];
static size_t g_expected;
static size_t g_received;
static bool g_receiving;

static void set_status(const char *state, const char *err)
{
	if (err != NULL && err[0] != '\0') {
		snprintf(g_status, sizeof(g_status),
			 "{\"state\":\"%s\",\"recv\":%u,\"size\":%u,\"err\":\"%s\"}", state,
			 (unsigned)g_received, (unsigned)g_expected, err);
	} else {
		snprintf(g_status, sizeof(g_status), "{\"state\":\"%s\",\"recv\":%u,\"size\":%u}",
			 state, (unsigned)g_received, (unsigned)g_expected);
	}
}

static bool parse_begin_size(const char *json, size_t *out_size)
{
	const char *key = strstr(json, "\"size\"");

	if (key == NULL) {
		return false;
	}
	key = strchr(key, ':');
	if (key == NULL) {
		return false;
	}
	*out_size = (size_t)strtoul(key + 1, NULL, 10);
	return *out_size > 0;
}

static ssize_t read_ctrl(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			 uint16_t len, uint16_t offset)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, g_status, strlen(g_status));
}

static void ota_finish_and_reboot(void)
{
	const uint8_t from = soft_reboot_boot_partition();
	const uint8_t target = (from == 0U) ? 1U : (from == 1U) ? 0U : 1U;

	set_status("done", NULL);
	g_receiving = false;
	if (ota_ab_finish_and_reboot(&g_flash_ctx) != 0) {
		set_status("error", "ab switch failed");
	}
}

static ssize_t write_ctrl(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			  uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0 || len == 0 || len >= 128) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	char json[128];

	memcpy(json, buf, len);
	json[len] = '\0';

	if (strstr(json, "\"op\":\"begin\"") != NULL) {
		size_t size = 0;

		if (!parse_begin_size(json, &size)) {
			set_status("error", "bad size");
			return len;
		}
		if (flash_img_init(&g_flash_ctx) != 0) {
			set_status("error", "flash init");
			return len;
		}
		g_expected = size;
		g_received = 0;
		g_receiving = true;
		set_status("receiving", NULL);
	} else if (strstr(json, "\"op\":\"abort\"") != NULL) {
		g_receiving = false;
		g_expected = 0;
		g_received = 0;
		set_status("idle", NULL);
	} else if (strstr(json, "\"op\":\"finish\"") != NULL || strstr(json, "\"op\":\"reboot\"") != NULL) {
		if (g_receiving) {
			ota_finish_and_reboot();
		} else {
			set_status("error", "not receiving");
		}
	}
	return len;
}

static ssize_t write_data(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			  uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);

	if (!g_receiving || len == 0) {
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}

	if (flash_img_buffered_write(&g_flash_ctx, buf, len, false) != 0) {
		set_status("error", "write");
		g_receiving = false;
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}

	g_received += len;
	if (g_received >= g_expected) {
		ota_finish_and_reboot();
	}
	return len;
}

BT_GATT_SERVICE_DEFINE(
	ota_svc, BT_GATT_PRIMARY_SERVICE(&ota_svc_uuid),
	BT_GATT_CHARACTERISTIC(&ota_ctrl_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, read_ctrl, write_ctrl, NULL),
	BT_GATT_CHARACTERISTIC(&ota_data_uuid.uuid, BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE, NULL, write_data, NULL), );

int ble_ota_gatt_init(void)
{
	set_status("idle", NULL);
	LOG_INF("BLE OTA service registered (A/B mcuboot)");
	return 0;
}
