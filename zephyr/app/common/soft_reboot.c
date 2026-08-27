/*
 * Controlled soft reboot reasons — persisted across reset, reported via crash ring
 * as non-fatal events (not panic/WDT crash ingest).
 */

#include "soft_reboot.h"

#include <esp_system.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>

#include "crash_report.h"

LOG_MODULE_REGISTER(soft_rb, LOG_LEVEL_INF);

#define SOFT_RB_MAGIC   0x534F5254U /* SORT */
#define SETTINGS_KEY    "soft_rb/pending"

struct soft_reboot_pending {
	uint32_t magic;
	uint8_t kind;
	uint8_t boot_part;
	uint8_t target_part;
	uint8_t pad;
	char ota_outcome[16];
	uint32_t crc32;
} __packed;

static struct soft_reboot_pending g_pending;
static bool g_pending_loaded;

static uint32_t pending_crc(const struct soft_reboot_pending *p)
{
	return crc32_ieee((const uint8_t *)p, offsetof(struct soft_reboot_pending, crc32));
}

static int settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *leaf = name;

	if (settings_name_steq(name, "pending", &leaf) && leaf == NULL) {
		if (len != sizeof(g_pending)) {
			return -EINVAL;
		}
		if (read_cb(cb_arg, &g_pending, sizeof(g_pending)) != (ssize_t)sizeof(g_pending)) {
			return -EIO;
		}
		g_pending_loaded = true;
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(soft_rb, "soft_rb", NULL, settings_set, NULL, NULL);

static void pending_clear(void)
{
	memset(&g_pending, 0, sizeof(g_pending));
	g_pending_loaded = false;
	(void)settings_delete(SETTINGS_KEY);
}

static int pending_save(void)
{
	g_pending.magic = SOFT_RB_MAGIC;
	g_pending.crc32 = pending_crc(&g_pending);
	return settings_save_one(SETTINGS_KEY, &g_pending, sizeof(g_pending));
}

static bool pending_valid(const struct soft_reboot_pending *p)
{
	return p->magic == SOFT_RB_MAGIC && p->crc32 == pending_crc(p) &&
	       p->kind != SOFT_REBOOT_NONE;
}

#if FIXED_PARTITION_EXISTS(slot0_partition)
#define SLOT0_AREA FIXED_PARTITION_ID(slot0_partition)
#else
#define SLOT0_AREA 0
#endif

#if FIXED_PARTITION_EXISTS(slot1_partition)
#define SLOT1_AREA FIXED_PARTITION_ID(slot1_partition)
#else
#define SLOT1_AREA 1
#endif

uint8_t soft_reboot_boot_partition(void)
{
#if defined(CONFIG_BOOTLOADER_MCUBOOT) && FIXED_PARTITION_EXISTS(slot0_partition)
	const int swap = mcuboot_swap_type();

	if (swap == BOOT_SWAP_TYPE_TEST || swap == BOOT_SWAP_TYPE_PERM) {
		return 1U;
	}
#endif
	return 0U;
}

const char *soft_reboot_partition_label(uint8_t part)
{
	switch (part) {
	case 0:
		return "A";
	case 1:
		return "B";
	default:
		return "?";
	}
}

const char *soft_reboot_kind_str(enum soft_reboot_kind kind)
{
	switch (kind) {
	case SOFT_REBOOT_BOOT_BTN:
		return "boot_btn";
	case SOFT_REBOOT_RTS:
		return "rts";
	case SOFT_REBOOT_FW_UPGRADE:
		return "fw_upgrade";
	default:
		return "unknown";
	}
}

void soft_reboot_schedule(enum soft_reboot_kind kind, uint8_t boot_part, uint8_t target_part)
{
	soft_reboot_schedule_ota(kind, boot_part, target_part, NULL);
}

void soft_reboot_schedule_ota(enum soft_reboot_kind kind, uint8_t boot_part, uint8_t target_part,
			      const char *ota_outcome)
{
	memset(&g_pending, 0, sizeof(g_pending));
	g_pending.kind = (uint8_t)kind;
	g_pending.boot_part = boot_part;
	g_pending.target_part = target_part;
	if (ota_outcome != NULL && ota_outcome[0] != '\0') {
		snprintf(g_pending.ota_outcome, sizeof(g_pending.ota_outcome), "%s", ota_outcome);
	}
	if (pending_save() != 0) {
		LOG_WRN("soft reboot schedule save failed kind=%u", (unsigned)kind);
	} else {
		LOG_INF("soft reboot scheduled kind=%s boot=%s target=%s",
			soft_reboot_kind_str(kind), soft_reboot_partition_label(boot_part),
			soft_reboot_partition_label(target_part));
	}
}

void soft_reboot_init(void)
{
	(void)settings_load_subtree("soft_rb");
}

static enum soft_reboot_kind reset_to_rts_kind(esp_reset_reason_t reason)
{
	switch (reason) {
	case ESP_RST_EXT:
		return SOFT_REBOOT_RTS;
	default:
		return SOFT_REBOOT_NONE;
	}
}

void soft_reboot_post_boot(void)
{
	enum soft_reboot_kind kind = SOFT_REBOOT_NONE;
	uint8_t boot_part = soft_reboot_boot_partition();
	uint8_t target_part = 255U;
	const char *ota_outcome = NULL;
	const esp_reset_reason_t reason = esp_reset_reason();

	if (g_pending_loaded && pending_valid(&g_pending)) {
		kind = (enum soft_reboot_kind)g_pending.kind;
		boot_part = g_pending.boot_part;
		target_part = g_pending.target_part;
		if (g_pending.ota_outcome[0] != '\0') {
			ota_outcome = g_pending.ota_outcome;
		}
		pending_clear();
	} else {
		kind = reset_to_rts_kind(reason);
	}

	if (kind == SOFT_REBOOT_NONE) {
		return;
	}

	crash_report_append_soft(soft_reboot_kind_str(kind), (uint8_t)reason, boot_part, target_part,
				 ota_outcome);
	LOG_INF("soft reboot recorded kind=%s reset=%u boot_part=%s",
		soft_reboot_kind_str(kind), (unsigned)reason,
		soft_reboot_partition_label(boot_part));
}
