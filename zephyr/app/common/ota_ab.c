/*
 * BLE OTA helper — writes incoming image to inactive slot (image-1) via flash_img.
 * Mcuboot A/B test boot + retry/rollback is implemented in code paths guarded by
 * CONFIG_BOOTLOADER_MCUBOOT; until the board is flashed with a full mcuboot layout
 * (bootloader @ 0x0 + signed app @ slot0), finish only stages the image and reboots.
 */

#include "ota_ab.h"

#include <esp_system.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/dfu/flash_img.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/reboot.h>

#include "crash_report.h"
#include "soft_reboot.h"

LOG_MODULE_REGISTER(ota_ab, LOG_LEVEL_INF);

#define OTA_AB_MAGIC     0x4F544142U /* OTAB */
#define SETTINGS_KEY     "ota_ab/state"
#define OTA_CONFIRM_MS   30000U
#define OTA_AB_MAX_TRIES 3U

struct ota_ab_state {
	uint32_t magic;
	uint8_t pending;
	uint8_t from_slot;
	uint8_t target_slot;
	uint8_t boot_tries;
	uint8_t target_bad;
	uint8_t pad[3];
	uint32_t crc32;
} __packed;

static struct ota_ab_state g_st;
static bool g_loaded;

static uint32_t st_crc(const struct ota_ab_state *s)
{
	return crc32_ieee((const uint8_t *)s, offsetof(struct ota_ab_state, crc32));
}

static int settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *leaf = name;

	if (settings_name_steq(name, "state", &leaf) && leaf == NULL) {
		if (len != sizeof(g_st)) {
			return -EINVAL;
		}
		if (read_cb(cb_arg, &g_st, sizeof(g_st)) != (ssize_t)sizeof(g_st)) {
			return -EIO;
		}
		g_loaded = true;
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(ota_ab, "ota_ab", NULL, settings_set, NULL, NULL);

static int state_save(void)
{
	g_st.magic = OTA_AB_MAGIC;
	g_st.crc32 = st_crc(&g_st);
	return settings_save_one(SETTINGS_KEY, &g_st, sizeof(g_st));
}

static bool state_valid(const struct ota_ab_state *s)
{
	return s->magic == OTA_AB_MAGIC && s->crc32 == st_crc(s);
}

static uint8_t inactive_target_slot(void)
{
	const uint8_t running = soft_reboot_boot_partition();

	return (running == 0U) ? 1U : 0U;
}

static void report_ota_outcome(const char *outcome, uint8_t from_slot, uint8_t target_slot)
{
	crash_report_append_soft("fw_upgrade", (uint8_t)esp_reset_reason(), from_slot, target_slot,
				 outcome);
}

void ota_ab_init(void)
{
	(void)settings_load_subtree("ota_ab");
}

bool ota_ab_target_marked_bad(uint8_t slot)
{
	return g_loaded && state_valid(&g_st) && g_st.target_bad != 0U && g_st.target_slot == slot;
}

#if defined(CONFIG_BOOTLOADER_MCUBOOT)
#include <zephyr/dfu/mcuboot.h>

void ota_ab_on_boot(void)
{
	const int swap = mcuboot_swap_type();

	if (!g_loaded || !state_valid(&g_st) || !g_st.pending) {
		return;
	}

	LOG_INF("ota_ab boot pending from=%s target=%s tries=%u swap=%d",
		soft_reboot_partition_label(g_st.from_slot),
		soft_reboot_partition_label(g_st.target_slot), g_st.boot_tries, swap);

	if (swap == BOOT_SWAP_TYPE_FAIL) {
		g_st.pending = 0U;
		g_st.target_bad = 1U;
		(void)state_save();
		report_ota_outcome("target_non_operable", g_st.from_slot, g_st.target_slot);
		return;
	}

	if (swap == BOOT_SWAP_TYPE_REVERT) {
		g_st.boot_tries++;
		if (g_st.boot_tries >= OTA_AB_MAX_TRIES) {
			g_st.pending = 0U;
			g_st.target_bad = 1U;
			(void)state_save();
			report_ota_outcome("target_non_operable", g_st.from_slot, g_st.target_slot);
			return;
		}
		if (boot_request_upgrade(BOOT_UPGRADE_TEST) != 0) {
			LOG_ERR("ota_ab retry boot_request_upgrade failed");
			return;
		}
		(void)state_save();
		return;
	}

	if (boot_is_img_confirmed()) {
		g_st.pending = 0U;
		(void)state_save();
		report_ota_outcome("transition_ok", g_st.from_slot, g_st.target_slot);
	}
}

void ota_ab_poll(void)
{
	if (!g_loaded || !state_valid(&g_st) || !g_st.pending || boot_is_img_confirmed()) {
		return;
	}
	if (k_uptime_get() < OTA_CONFIRM_MS) {
		return;
	}
	if (boot_write_img_confirmed() != 0) {
		LOG_WRN("ota_ab confirm failed");
		return;
	}
	g_st.pending = 0U;
	(void)state_save();
	report_ota_outcome("transition_ok", g_st.from_slot, g_st.target_slot);
}

#else /* !CONFIG_BOOTLOADER_MCUBOOT */

void ota_ab_on_boot(void)
{
}

void ota_ab_poll(void)
{
}

#endif /* CONFIG_BOOTLOADER_MCUBOOT */

int ota_ab_finish_and_reboot(struct flash_img_context *ctx)
{
	const uint8_t from = soft_reboot_boot_partition();
	const uint8_t target = inactive_target_slot();

	if (ota_ab_target_marked_bad(target)) {
		LOG_ERR("ota_ab: target %s marked non-operable", soft_reboot_partition_label(target));
		return -EPERM;
	}

	if (ctx != NULL && flash_img_buffered_write(ctx, NULL, 0, true) != 0) {
		return -EIO;
	}

	memset(&g_st, 0, sizeof(g_st));
	g_st.pending = 1U;
	g_st.from_slot = from;
	g_st.target_slot = target;
	(void)state_save();

#if defined(CONFIG_BOOTLOADER_MCUBOOT)
	if (boot_request_upgrade(BOOT_UPGRADE_TEST) != 0) {
		LOG_ERR("ota_ab boot_request_upgrade failed");
		return -EIO;
	}
#endif

	soft_reboot_schedule(SOFT_REBOOT_FW_UPGRADE, from, target);
	LOG_INF("ota_ab staged %u bytes for %s → %s — reboot",
		ctx != NULL ? (unsigned)flash_img_bytes_written(ctx) : 0U,
		soft_reboot_partition_label(from), soft_reboot_partition_label(target));

	k_msleep(200);
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}
