#include "ble_config_gatt.h"

#include <string.h>

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>

#include "device_config.h"
#include "imu_pipeline.h"
#include "vibro_capture.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(ble_cfg, LOG_LEVEL_INF);

#define BT_UUID_CONFIG_SVC_VAL \
	BT_UUID_128_ENCODE(0x4a6e0101, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)
#define BT_UUID_CONFIG_DATA_VAL \
	BT_UUID_128_ENCODE(0x4a6e0102, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)
#define BT_UUID_CONFIG_CMD_VAL \
	BT_UUID_128_ENCODE(0x4a6e0103, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)
/** Read-only: compact JSON list of the 5 reference-profile slots (name/
 * duration/rms/valid + which one is active). See vibro_ref_store.h. */
#define BT_UUID_CONFIG_REFLIST_VAL \
	BT_UUID_128_ENCODE(0x4a6e0104, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)

static struct bt_uuid_128 cfg_svc_uuid = BT_UUID_INIT_128(BT_UUID_CONFIG_SVC_VAL);
static struct bt_uuid_128 cfg_data_uuid = BT_UUID_INIT_128(BT_UUID_CONFIG_DATA_VAL);
static struct bt_uuid_128 cfg_cmd_uuid = BT_UUID_INIT_128(BT_UUID_CONFIG_CMD_VAL);
static struct bt_uuid_128 cfg_reflist_uuid = BT_UUID_INIT_128(BT_UUID_CONFIG_REFLIST_VAL);

static char g_reflist_json[640];
static size_t g_reflist_len;

static struct device_config_v1 g_cfg;

static atomic_t g_erase_reboot_pending;
static int64_t g_erase_reboot_deadline;

static void erase_nvs_and_reboot_now(void)
{
	if (device_config_storage_erase()) {
		device_config_defaults(&g_cfg);
		if (!device_config_save_sync(&g_cfg)) {
			LOG_ERR("devcfg post-erase save failed — defaults apply in RAM until next boot");
		}
	} else {
		LOG_ERR("devcfg NVS erase failed — rebooting without wipe");
	}
	LOG_WRN("devcfg: sys_reboot after NVS erase");
	k_msleep(100);
	sys_reboot(SYS_REBOOT_COLD);
}

static void reload_cfg(void)
{
	(void)device_config_load(&g_cfg);
}

static ssize_t read_data(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			 uint16_t len, uint16_t offset)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	reload_cfg();
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &g_cfg, sizeof(g_cfg));
}

static ssize_t write_data(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			  uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0 || len != sizeof(struct device_config_v1)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	const enum device_config_apply_result ar = device_config_apply_remote(buf);

	if (ar == DEVICE_CONFIG_APPLY_STALE) {
		return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
	}
	if (ar != DEVICE_CONFIG_APPLY_OK) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}
	reload_cfg();
	imu_pipeline_apply_config();
	return len;
}

static ssize_t read_reflist(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			    uint16_t len, uint16_t offset)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);

	/* Rebuild only at the start of a read sequence — see the STATUS/DATA
	 * torn-read comment in ble_imu_gatt.c for why a multi-part ATT long
	 * read must stay on one generation of the buffer for its whole
	 * transfer. This store isn't touched by any background poll, so a
	 * single static buffer (no double-buffering) is sufficient. */
	if (offset == 0U) {
		const int n = vibro_capture_list_references_json(g_reflist_json,
								  sizeof(g_reflist_json));

		g_reflist_len = (n > 0) ? (size_t)n : 0U;
	}
	return bt_gatt_attr_read(conn, attr, buf, len, offset, g_reflist_json, g_reflist_len);
}

static ssize_t write_cmd(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			 uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0 || len == 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	const uint8_t *bytes = buf;
	const uint8_t cmd = bytes[0];

	switch (cmd) {
	case 0:
		reload_cfg();
		imu_pipeline_apply_config();
		break;
	case 1:
		reload_cfg();
		imu_pipeline_apply_config();
		LOG_INF("devcfg commit applied");
		break;
	case 3: {
		/* Payload: [3][slot][name...] — slot/name optional for back-compat
		 * (bare [3] == slot 0, auto-named). */
		const uint8_t slot = (len >= 2) ? bytes[1] : 0U;
		char name[VIBRO_REF_NAME_MAX];

		name[0] = '\0';
		if (len > 2) {
			const size_t name_len = MIN((size_t)(len - 2), sizeof(name) - 1U);

			memcpy(name, &bytes[2], name_len);
			name[name_len] = '\0';
		}
		(void)vibro_capture_start_reference(slot, name);
		break;
	}
	case 4:
		(void)vibro_capture_stop_reference();
		LOG_INF("vibro reference stopped len=%u", (unsigned)vibro_capture_reference_len());
		break;
	case 7:
		if (len >= 2) {
			const int err = vibro_capture_select_reference(bytes[1]);

			LOG_INF("vibro reference select slot=%u -> %d", bytes[1], err);
		}
		break;
	case 8:
		if (len >= 2) {
			const int err = vibro_capture_delete_reference(bytes[1]);

			LOG_INF("vibro reference delete slot=%u -> %d", bytes[1], err);
		}
		break;
	case 5:
		if (len >= 5) {
			const uint32_t seq = (uint32_t)bytes[1] | ((uint32_t)bytes[2] << 8) |
					     ((uint32_t)bytes[3] << 16) | ((uint32_t)bytes[4] << 24);

			if (vibro_capture_ack_offload(seq)) {
				LOG_INF("offload ACK seq=%u — ring rotated", seq);
			} else {
				LOG_WRN("offload ACK seq=%u ignored", seq);
			}
		}
		break;
	case 6:
		LOG_WRN("devcfg erase NVS scheduled (reboot in 500ms, main poll)");
		g_erase_reboot_deadline = k_uptime_get() + 500;
		atomic_set(&g_erase_reboot_pending, 1);
		break;
	default:
		break;
	}
	return len;
}

BT_GATT_SERVICE_DEFINE(
	cfg_svc, BT_GATT_PRIMARY_SERVICE(&cfg_svc_uuid),
	BT_GATT_CHARACTERISTIC(&cfg_data_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, read_data, write_data, NULL),
	BT_GATT_CHARACTERISTIC(&cfg_cmd_uuid.uuid, BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE, NULL,
			       write_cmd, NULL),
	BT_GATT_CHARACTERISTIC(&cfg_reflist_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
			       read_reflist, NULL, NULL), );

int ble_config_gatt_init(void)
{
	atomic_set(&g_erase_reboot_pending, 0);
	reload_cfg();
	LOG_INF("BLE config service registered");
	return 0;
}

void ble_config_gatt_poll(void)
{
	if (!atomic_get(&g_erase_reboot_pending)) {
		return;
	}

	if (k_uptime_get() < g_erase_reboot_deadline) {
		return;
	}

	atomic_set(&g_erase_reboot_pending, 0);
	erase_nvs_and_reboot_now();
}
