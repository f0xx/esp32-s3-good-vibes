#include "ble_config_gatt.h"

#include <string.h>

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>

#include "battery_bench.h"
#include "device_config.h"
#include "flash_safety.h"
#include "floor_calib.h"
#include "imu_pipeline.h"
#include "soft_reboot.h"
#include "vibro_led.h"
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
/** Read-only: flat-floor mounting calibration status JSON — see floor_calib.h. */
#define BT_UUID_CONFIG_FLOORCAL_VAL \
	BT_UUID_128_ENCODE(0x4a6e0105, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)

static struct bt_uuid_128 cfg_svc_uuid = BT_UUID_INIT_128(BT_UUID_CONFIG_SVC_VAL);
static struct bt_uuid_128 cfg_data_uuid = BT_UUID_INIT_128(BT_UUID_CONFIG_DATA_VAL);
static struct bt_uuid_128 cfg_cmd_uuid = BT_UUID_INIT_128(BT_UUID_CONFIG_CMD_VAL);
static struct bt_uuid_128 cfg_reflist_uuid = BT_UUID_INIT_128(BT_UUID_CONFIG_REFLIST_VAL);
static struct bt_uuid_128 cfg_floorcal_uuid = BT_UUID_INIT_128(BT_UUID_CONFIG_FLOORCAL_VAL);

static char g_reflist_json[640];
static size_t g_reflist_len;
static char g_floorcal_json[128];
static size_t g_floorcal_len;

static struct device_config_v1 g_cfg;

static atomic_t g_erase_reboot_pending;
static int64_t g_erase_reboot_deadline;
/* Bounded wait for app_flash_erase_safe() before the factory-reset NVS erase — see
 * flash_safety.h. This is a rare, deliberate action that already reboots right after, so
 * unlike the other stores we don't need a full deferred-ram pattern: just give BLE a chance
 * to go idle first, but don't block a user-requested factory reset forever if it doesn't
 * (e.g. app never disconnects) — bail out and erase anyway once the cap is hit. */
#define DEVCFG_ERASE_SAFE_WAIT_CAP_MS 5000

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
	soft_reboot_schedule(SOFT_REBOOT_BOOT_BTN, soft_reboot_boot_partition(), 255U);
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
	if (battery_bench_config_locked()) {
		return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
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

static ssize_t read_floorcal(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			     uint16_t len, uint16_t offset)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);

	if (offset == 0U) {
		const int n = floor_calib_status_json(g_floorcal_json, sizeof(g_floorcal_json));

		g_floorcal_len = (n > 0) ? (size_t)n : 0U;
	}
	return bt_gatt_attr_read(conn, attr, buf, len, offset, g_floorcal_json, g_floorcal_len);
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
	if (battery_bench_config_locked()) {
		return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
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
	case 9:
		(void)vibro_capture_clear_all_references();
		device_config_set_vibro_armed(false);
		LOG_WRN("vibro reference: all slots cleared (CMD 9)");
		break;
	case 10:
		device_config_set_vibro_armed(true);
		LOG_INF("vibro monitoring armed (CMD 10) — acrylic LED operational");
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
	case 11: {
		/* Payload: [11][duration_ms lo][duration_ms hi] — duration optional
		 * (bare [11] == FLOOR_CALIB_DEFAULT_DURATION_MS). Device must be held
		 * still on a true-level reference for the whole window. */
		const uint16_t duration_ms =
			(len >= 3) ? (uint16_t)(bytes[1] | ((uint16_t)bytes[2] << 8)) : 0U;

		floor_calib_start(duration_ms);
		break;
	}
	case 12:
		floor_calib_clear();
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
			       read_reflist, NULL, NULL),
	BT_GATT_CHARACTERISTIC(&cfg_floorcal_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
			       read_floorcal, NULL, NULL), );

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

	if (!app_flash_erase_safe() &&
	    k_uptime_get() < g_erase_reboot_deadline + DEVCFG_ERASE_SAFE_WAIT_CAP_MS) {
		return;
	}
	if (!app_flash_erase_safe()) {
		LOG_WRN("devcfg erase: BLE still active after %dms wait cap — erasing anyway",
			DEVCFG_ERASE_SAFE_WAIT_CAP_MS);
	}

	atomic_set(&g_erase_reboot_pending, 0);
	erase_nvs_and_reboot_now();
}
