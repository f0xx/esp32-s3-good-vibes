#include "ble_ota_gatt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

#include "ble_imu_gatt.h"
#include "display_panel.h"
#include "flash_safety.h"
#include "mt200_bridge.h"
#include "ota_ab.h"
#include "scene_live.h"
#include "soft_reboot.h"
#include "stall_watchdog.h"

LOG_MODULE_REGISTER(ble_ota, LOG_LEVEL_INF);

#define BT_UUID_OTA_SVC_VAL \
	BT_UUID_128_ENCODE(0x4a6e0201, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)
#define BT_UUID_OTA_CTRL_VAL \
	BT_UUID_128_ENCODE(0x4a6e0202, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)
#define BT_UUID_OTA_DATA_VAL \
	BT_UUID_128_ENCODE(0x4a6e0203, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)

/* flash_img always programs slot1_partition (secondary). */
#define OTA_UPLOAD_AREA_ID PARTITION_ID(slot1_partition)
/* One SPI flash sector — matches flash_safety / TG0WDT lessons. */
#define OTA_ERASE_CHUNK    0x1000U
#define OTA_TRAILER_PAD    0x2000U /* McUboot trailer / alignment margin */
#define OTA_FAIL_HOLD_MS   5000U
#define OTA_REBOOT_HOLD_MS 800U

static struct bt_uuid_128 ota_svc_uuid = BT_UUID_INIT_128(BT_UUID_OTA_SVC_VAL);
static struct bt_uuid_128 ota_ctrl_uuid = BT_UUID_INIT_128(BT_UUID_OTA_CTRL_VAL);
static struct bt_uuid_128 ota_data_uuid = BT_UUID_INIT_128(BT_UUID_OTA_DATA_VAL);

static struct flash_img_context g_flash_ctx;
static char g_status[128];
static size_t g_expected;
static size_t g_received;

enum ota_phase {
	OTA_IDLE = 0,
	OTA_WAIT_DISC,
	OTA_ERASING,
	OTA_READY,
	OTA_RECEIVING,
	OTA_FAIL_HOLD,
	OTA_REBOOT_HOLD,
};

static enum ota_phase g_phase;
static bool g_hold_adv;
static bool g_load_shed;
static off_t g_erase_off;
static size_t g_erase_total;
static const struct flash_area *g_erase_fa;
static int64_t g_fail_until_ms;
static int64_t g_reboot_at_ms;

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

static void ota_clear_hold(void)
{
	if (!g_hold_adv) {
		return;
	}
	g_hold_adv = false;
	ble_imu_set_hold_adv(false);
}

static void ota_load_shed_on(void)
{
	if (g_load_shed) {
		return;
	}
	g_load_shed = true;
	mt200_bridge_pause();
	ble_imu_gatt_set_traffic_paused(true);
	LOG_INF("OTA load-shed: MT200 + IMU BLE notify paused");
}

static void ota_load_shed_off(void)
{
	if (!g_load_shed) {
		return;
	}
	g_load_shed = false;
	ble_imu_gatt_set_traffic_paused(false);
	mt200_bridge_resume();
	LOG_INF("OTA load-shed cleared");
}

static void ota_enter_fail(const char *err)
{
	if (g_erase_fa != NULL) {
		flash_area_close(g_erase_fa);
		g_erase_fa = NULL;
	}
	g_phase = OTA_FAIL_HOLD;
	g_fail_until_ms = k_uptime_get() + OTA_FAIL_HOLD_MS;
	g_erase_off = 0;
	g_erase_total = 0;
	ota_clear_hold();
	/* Keep load-shed during FAIL so the panel stays calm; cleared when hold ends. */
	set_status("error", err != NULL ? err : "fail");
	LOG_WRN("OTA FAIL hold %ums (%s)", OTA_FAIL_HOLD_MS, err ? err : "?");
}

static void ota_abort_to_idle(const char *err)
{
	if (err != NULL && err[0] != '\0') {
		ota_enter_fail(err);
		return;
	}
	if (g_erase_fa != NULL) {
		flash_area_close(g_erase_fa);
		g_erase_fa = NULL;
	}
	g_phase = OTA_IDLE;
	g_expected = 0;
	g_received = 0;
	g_erase_off = 0;
	g_erase_total = 0;
	g_fail_until_ms = 0;
	g_reboot_at_ms = 0;
	ota_clear_hold();
	ota_load_shed_off();
	set_status("idle", NULL);
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

static int ota_start_receiving(void)
{
	if (flash_img_init(&g_flash_ctx) != 0) {
		ota_abort_to_idle("flash init");
		return -EIO;
	}
	g_received = 0;
	g_phase = OTA_RECEIVING;
	ota_load_shed_on();
	/* Adv must be up for the phone link; do not hold. */
	ota_clear_hold();
	set_status("receiving", NULL);
	LOG_INF("OTA receiving size=%u (slot pre-erased, no live erase)", (unsigned)g_expected);
	return 0;
}

static void ota_begin_erase(size_t size)
{
	g_expected = size;
	g_received = 0;
	g_erase_off = 0;
	g_erase_total = 0;
	g_phase = OTA_WAIT_DISC;
	ota_load_shed_on();
	set_status("erasing", NULL);
	if (!g_hold_adv) {
		g_hold_adv = true;
		ble_imu_set_hold_adv(true);
	}
	ble_imu_disconnect_phone_for_wifi();
	LOG_INF("OTA begin size=%u — drop BLE, erase slot1, then ready", (unsigned)size);
}

static void ota_request_reboot(void)
{
	if (g_phase == OTA_REBOOT_HOLD) {
		return;
	}
	g_phase = OTA_REBOOT_HOLD;
	g_reboot_at_ms = k_uptime_get() + OTA_REBOOT_HOLD_MS;
	set_status("rebooting", NULL);
	LOG_INF("OTA REBOOT UI — soft fw_upgrade then cold reboot");
}

static void ota_do_finish_reboot(void)
{
	if (ota_ab_finish_and_reboot(&g_flash_ctx) != 0) {
		ota_enter_fail("ab switch failed");
	}
}

static ssize_t read_ctrl(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			 uint16_t len, uint16_t offset)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, g_status, strlen(g_status));
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
			ota_abort_to_idle("bad size");
			return len;
		}
		if (g_phase == OTA_FAIL_HOLD || g_phase == OTA_REBOOT_HOLD) {
			return len;
		}
		/* Reconnect after slot erase — image area is blank; start writes.
		 * Phone may re-begin with a slightly different CDN size; if it still
		 * fits the wiped region, do not re-erase (second erase was TG0WDT). */
		if (g_phase == OTA_READY) {
			const size_t need =
				(size + OTA_TRAILER_PAD + (OTA_ERASE_CHUNK - 1U)) &
				~(OTA_ERASE_CHUNK - 1U);

			if (need <= g_erase_total) {
				if (size != g_expected) {
					LOG_INF("OTA begin size %u → %u (fits erased %u) — no re-erase",
						(unsigned)g_expected, (unsigned)size,
						(unsigned)g_erase_total);
				}
				g_expected = size;
				(void)ota_start_receiving();
				return len;
			}
			LOG_INF("OTA begin size %u → %u needs %u > erased %u — re-erase",
				(unsigned)g_expected, (unsigned)size, (unsigned)need,
				(unsigned)g_erase_total);
		}
		if (g_phase == OTA_WAIT_DISC || g_phase == OTA_ERASING) {
			g_expected = size;
			set_status("erasing", NULL);
			return len;
		}
		ota_begin_erase(size);
	} else if (strstr(json, "\"op\":\"abort\"") != NULL) {
		/* Last DATA chunk already arms REBOOT_HOLD; ignore late abort/reboot races. */
		if (g_phase == OTA_REBOOT_HOLD || g_phase == OTA_FAIL_HOLD) {
			return len;
		}
		ota_abort_to_idle("abort");
	} else if (strstr(json, "\"op\":\"finish\"") != NULL || strstr(json, "\"op\":\"reboot\"") != NULL) {
		if (g_phase == OTA_REBOOT_HOLD) {
			/* Phone always sends reboot after the last chunk; FW already
			 * entered REBOOT_HOLD from g_received >= g_expected. */
			return len;
		}
		if (g_phase == OTA_RECEIVING) {
			ota_request_reboot();
		} else if (g_phase != OTA_FAIL_HOLD) {
			ota_abort_to_idle("not receiving");
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

	if (g_phase != OTA_RECEIVING || len == 0) {
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}

	stall_watchdog_feed_main();
	if (flash_img_buffered_write(&g_flash_ctx, buf, len, false) != 0) {
		LOG_ERR("OTA flash write failed at recv=%u +%u", (unsigned)g_received, len);
		ota_abort_to_idle("write");
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}

	g_received += len;
	set_status("receiving", NULL);
	/* Log every ~64 KiB and on completion — serial progress during receive. */
	if ((g_received & 0xffffU) < (size_t)len || g_received >= g_expected) {
		const unsigned pct =
			g_expected > 0U ? (unsigned)(((uint64_t)g_received * 100U) / g_expected) : 0U;
		LOG_INF("OTA recv %u/%u (%u%%)", (unsigned)g_received, (unsigned)g_expected, pct);
	}
	if (g_received >= g_expected) {
		ota_request_reboot();
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
	g_phase = OTA_IDLE;
	set_status("idle", NULL);
	LOG_INF("BLE OTA service registered (A/B mcuboot, pre-erase slot)");
	return 0;
}

bool ble_ota_ui_active(void)
{
	return g_phase != OTA_IDLE;
}

bool ble_ota_ui_snapshot(const char **label, uint16_t *label_rgb565, uint16_t *bar_rgb565,
			 uint16_t *progress_permille)
{
	uint16_t pct = 0;

	if (label == NULL || label_rgb565 == NULL || bar_rgb565 == NULL ||
	    progress_permille == NULL) {
		return false;
	}

	switch (g_phase) {
	case OTA_IDLE:
		return false;
	case OTA_FAIL_HOLD:
		*label = "FAIL";
		*label_rgb565 = PANEL_RED;
		*bar_rgb565 = PANEL_RED;
		*progress_permille = 0;
		return true;
	case OTA_REBOOT_HOLD:
		*label = "REBOOT";
		*label_rgb565 = PANEL_GREEN;
		*bar_rgb565 = PANEL_GREEN;
		*progress_permille = 1000;
		return true;
	default:
		break;
	}

	*label = "Upgrading FW...";
	*label_rgb565 = PANEL_WHITE;
	*bar_rgb565 = PANEL_BLUE;

	if (g_phase == OTA_WAIT_DISC) {
		pct = 0;
	} else if (g_phase == OTA_ERASING && g_erase_total > 0) {
		/* Erase occupies 0..300‰ of the bar. */
		pct = (uint16_t)(((uint64_t)g_erase_off * 300U) / g_erase_total);
	} else if (g_phase == OTA_READY) {
		pct = 300;
	} else if (g_phase == OTA_RECEIVING && g_expected > 0) {
		pct = (uint16_t)(300U + (((uint64_t)g_received * 700U) / g_expected));
		if (pct > 1000U) {
			pct = 1000U;
		}
	}
	*progress_permille = pct;
	return true;
}

void ble_ota_gatt_tick(void)
{
	int err;
	const int64_t now = k_uptime_get();

	if (g_phase == OTA_FAIL_HOLD) {
		if (now >= g_fail_until_ms) {
			g_phase = OTA_IDLE;
			g_expected = 0;
			g_received = 0;
			g_fail_until_ms = 0;
			ota_load_shed_off();
			set_status("idle", NULL);
			LOG_INF("OTA FAIL hold done — cube scene restored");
		}
		return;
	}

	if (g_phase == OTA_REBOOT_HOLD) {
		if (now >= g_reboot_at_ms) {
			ota_do_finish_reboot();
		}
		return;
	}

	if (g_phase == OTA_WAIT_DISC) {
		/* Same 4s post-disconnect gate as vibro/crash flash erases — 600ms
		 * was TG0WDT on the first sector every time (ttyACM0.log). */
		if (!app_flash_erase_safe()) {
			return;
		}
		err = flash_area_open(OTA_UPLOAD_AREA_ID, &g_erase_fa);
		if (err != 0 || g_erase_fa == NULL) {
			LOG_ERR("OTA flash_area_open failed (%d)", err);
			ota_abort_to_idle("flash open");
			return;
		}
		{
			size_t need = g_expected + OTA_TRAILER_PAD;

			need = (need + (OTA_ERASE_CHUNK - 1U)) & ~(OTA_ERASE_CHUNK - 1U);
			if (need > g_erase_fa->fa_size) {
				need = g_erase_fa->fa_size;
			}
			if (need < OTA_ERASE_CHUNK) {
				need = OTA_ERASE_CHUNK;
			}
			g_erase_total = need;
		}
		g_erase_off = 0;
		g_phase = OTA_ERASING;
		LOG_INF("OTA erasing slot1 %u/%u (image+pad, flash-quiet)",
			(unsigned)g_erase_total, (unsigned)g_erase_fa->fa_size);
		return;
	}

	if (g_phase != OTA_ERASING || g_erase_fa == NULL) {
		return;
	}

	stall_watchdog_feed_main();
	{
		size_t chunk = OTA_ERASE_CHUNK;

		if ((size_t)g_erase_off + chunk > g_erase_total) {
			chunk = g_erase_total - (size_t)g_erase_off;
		}
		scene_live_set_flash_quiet(true);
		/* Let render thread notice the quiet flag before cache-disable erase. */
		k_msleep(30);
		stall_watchdog_feed_main();
		err = flash_area_erase(g_erase_fa, g_erase_off, chunk);
		scene_live_set_flash_quiet(false);
		stall_watchdog_feed_main();
		if (err != 0) {
			LOG_ERR("OTA erase failed off=0x%lx (%d)", (unsigned long)g_erase_off, err);
			ota_abort_to_idle("erase");
			return;
		}
		g_erase_off += (off_t)chunk;
		if ((g_erase_off & 0xffff) == 0 || (size_t)g_erase_off >= g_erase_total) {
			LOG_INF("OTA erase %u/%u", (unsigned)g_erase_off, (unsigned)g_erase_total);
		}
	}

	if ((size_t)g_erase_off < g_erase_total) {
		return;
	}

	flash_area_close(g_erase_fa);
	g_erase_fa = NULL;
	g_phase = OTA_READY;
	ota_clear_hold();
	set_status("ready", NULL);
	LOG_INF("OTA slot1 erased — advertising for resume begin");
}
