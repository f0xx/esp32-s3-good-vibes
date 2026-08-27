/*
 * Crash report — reset reason, flash coredump, NVS ring (step 3b).
 */

#include "crash_report.h"

#include <esp_system.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/debug/coredump.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "crash_ring_store.h"
#include "crash_rtc_capture.h"
#include "fw_version.h"
#include "power_manager.h"

LOG_MODULE_REGISTER(crash_rpt, LOG_LEVEL_INF);

#define CRASH_PARSE_BUF  512U

static uint8_t g_reset_code;
static struct crash_report_info g_pending;
static bool g_has_pending;
static int8_t g_active_slot = -1;
static uint8_t g_dump_buf[CRASH_PARSE_BUF];
static bool g_boot_crash_deferred;
static esp_reset_reason_t g_boot_reset_reason;

static const char *reset_reason_str(esp_reset_reason_t reason)
{
	switch (reason) {
	case ESP_RST_POWERON:
		return "poweron";
	case ESP_RST_EXT:
		return "ext";
	case ESP_RST_SW:
		return "sw";
	case ESP_RST_PANIC:
		return "panic";
	case ESP_RST_INT_WDT:
		return "int_wdt";
	case ESP_RST_TASK_WDT:
		return "task_wdt";
	case ESP_RST_WDT:
		return "wdt";
	case ESP_RST_DEEPSLEEP:
		return "deepsleep";
	case ESP_RST_BROWNOUT:
		return "brownout";
	case ESP_RST_SDIO:
		return "sdio";
	case ESP_RST_UNKNOWN:
		return "unknown";
	default:
		return "other";
	}
}

static bool reset_is_voluntary(esp_reset_reason_t reason)
{
	switch (reason) {
	case ESP_RST_SW:
	case ESP_RST_EXT:
	case ESP_RST_DEEPSLEEP:
	case ESP_RST_POWERON:
	case ESP_RST_UNKNOWN:
		return true;
	default:
		return false;
	}
}

static bool reset_suggests_crash(esp_reset_reason_t reason)
{
	switch (reason) {
	case ESP_RST_PANIC:
	case ESP_RST_INT_WDT:
	case ESP_RST_TASK_WDT:
	case ESP_RST_WDT:
	case ESP_RST_BROWNOUT:
		return true;
	default:
		return false;
	}
}

static bool parse_arch_pc(const uint8_t *buf, size_t len, struct crash_report_info *out)
{
	for (size_t i = 0; i + 24U < len; i++) {
		if (buf[i] != COREDUMP_ARCH_HDR_ID) {
			continue;
		}

		const uint8_t *blk = &buf[i + sizeof(struct coredump_arch_hdr_t)];

		if (i + sizeof(struct coredump_arch_hdr_t) + 16U > len) {
			continue;
		}

		out->pc = sys_get_le32(blk + 4U);
		out->exccause = sys_get_le32(blk + 8U);
		out->excvaddr = sys_get_le32(blk + 12U);
		return out->pc != 0U;
	}

	return false;
}

static void fill_pending_from_reset(struct crash_report_info *out, esp_reset_reason_t reason)
{
	memset(out, 0, sizeof(*out));
	out->valid = true;
	out->soft = false;
	out->reset_reason = (uint8_t)reason;
	out->reason = reset_reason_str(reason);
	out->uptime_ms = k_uptime_get_32();
	snprintf(out->fw_version, sizeof(out->fw_version), "%s", FW_VERSION_NAME);
}

static bool scan_stored_coredump(struct crash_report_info *out)
{
#if defined(CONFIG_DEBUG_COREDUMP) && defined(CONFIG_DEBUG_COREDUMP_BACKEND_FLASH_PARTITION)
	int has = coredump_query(COREDUMP_QUERY_HAS_STORED_DUMP, NULL);
	int dump_size = 0;
	struct coredump_cmd_copy_arg copy_arg = { 0 };

	if (has <= 0) {
		return false;
	}

	dump_size = coredump_query(COREDUMP_QUERY_GET_STORED_DUMP_SIZE, NULL);
	if (dump_size <= 0) {
		return false;
	}

	copy_arg.offset = 0;
	copy_arg.buffer = g_dump_buf;
	copy_arg.length = MIN((size_t)dump_size, sizeof(g_dump_buf));
	if (coredump_cmd(COREDUMP_CMD_COPY_STORED_DUMP, &copy_arg) <= 0) {
		LOG_WRN("coredump copy failed");
		return false;
	}

	fill_pending_from_reset(out, esp_reset_reason());
	out->dump_size = (uint32_t)dump_size;

	if (g_dump_buf[0] == 'Z' && g_dump_buf[1] == 'E') {
		out->reason = "fatal";
	}

	(void)parse_arch_pc(g_dump_buf, copy_arg.length, out);

	if (out->pc != 0U) {
		out->backtrace[0] = out->pc;
		out->backtrace_count = 1;
	}

	LOG_WRN("stored coredump size=%u pc=0x%08x cause=%u", out->dump_size, out->pc,
		out->exccause);
	return true;
#else
	ARG_UNUSED(out);
	return false;
#endif
}

/* Folds in exccause/excvaddr captured by crash_rtc_capture.c (RTC SRAM, no flash involved)
 * for a genuine Zephyr-caught CPU exception on the *previous* boot — see that module's doc
 * comment for exactly which crash classes this can and can't cover. No-op (leaves `info`
 * untouched) if there's nothing valid to consume, which is the common case. */
static void enrich_from_rtc_capture(struct crash_report_info *info)
{
	struct crash_rtc_capture cap;

	if (!crash_rtc_capture_consume(&cap)) {
		return;
	}

	info->exccause = cap.exccause;
	info->excvaddr = cap.excvaddr;
	LOG_WRN("crash detail recovered from RTC capture: reason=%u exccause=%u excvaddr=0x%08x "
		"thread=%s", cap.reason, cap.exccause, cap.excvaddr, cap.thread_name);
}

static void persist_boot_crash(esp_reset_reason_t reason)
{
	struct crash_report_info info;
	struct crash_ring_telemetry tel;
	bool has_info = false;

	if (scan_stored_coredump(&info)) {
		has_info = true;
	} else if (reset_suggests_crash(reason)) {
		fill_pending_from_reset(&info, reason);
		has_info = true;
	}

	if (!has_info) {
		/* Even if the reset reason alone didn't look crash-worthy, an RTC capture
		 * existing means a Zephyr fatal error definitely happened right before this
		 * boot — don't drop that on the floor just because esp_reset_reason() came
		 * back with something reset_suggests_crash() doesn't recognize. */
		struct crash_rtc_capture cap;

		if (crash_rtc_capture_consume(&cap)) {
			fill_pending_from_reset(&info, reason);
			info.exccause = cap.exccause;
			info.excvaddr = cap.excvaddr;
			has_info = true;
			LOG_WRN("crash detail recovered from RTC capture (reset reason was not "
				"self-evidently a crash): exccause=%u excvaddr=0x%08x thread=%s",
				cap.exccause, cap.excvaddr, cap.thread_name);
		} else {
			return;
		}
	} else {
		enrich_from_rtc_capture(&info);
	}

	power_manager_telemetry_snapshot(&tel);
	const int slot = crash_ring_append(&info, &tel);

	if (slot >= 0) {
		g_active_slot = (int8_t)slot;
		memcpy(&g_pending, &info, sizeof(g_pending));
		g_has_pending = true;
	}

#if defined(CONFIG_DEBUG_COREDUMP) && defined(CONFIG_DEBUG_COREDUMP_BACKEND_FLASH_PARTITION)
	(void)coredump_cmd(COREDUMP_CMD_INVALIDATE_STORED_DUMP, NULL);
#endif
}

void crash_report_append_soft(const char *soft_reason, uint8_t reset_reason, uint8_t boot_part,
			      uint8_t target_part, const char *ota_outcome)
{
	struct crash_report_info info;
	struct crash_ring_telemetry tel;
	char reason_buf[16];

	if (soft_reason == NULL || soft_reason[0] == '\0') {
		return;
	}

	snprintf(reason_buf, sizeof(reason_buf), "soft:%s", soft_reason);
	fill_pending_from_reset(&info, (esp_reset_reason_t)reset_reason);
	info.reason = reason_buf;
	info.pc = 0U;
	info.exccause = 0U;
	info.excvaddr = 0U;
	info.backtrace_count = 0U;
	info.dump_size = 0U;
	info.soft = true;
	info.boot_part = boot_part;
	info.target_part = target_part;
	if (ota_outcome != NULL && ota_outcome[0] != '\0') {
		snprintf(info.ota_outcome, sizeof(info.ota_outcome), "%s", ota_outcome);
	}

	power_manager_telemetry_snapshot(&tel);
	const int slot = crash_ring_append_soft(&info, &tel);

	if (slot >= 0) {
		g_active_slot = (int8_t)slot;
		memcpy(&g_pending, &info, sizeof(g_pending));
		g_has_pending = true;
	}
}

void crash_report_init(void)
{
	const esp_reset_reason_t reason = esp_reset_reason();

	g_reset_code = (uint8_t)reason;
	g_has_pending = false;
	g_active_slot = -1;
	memset(&g_pending, 0, sizeof(g_pending));

	(void)crash_ring_init();

	LOG_INF("last chip reset: %s (%u)", reset_reason_str(reason), (unsigned)reason);

	if (reset_suggests_crash(reason)) {
		g_boot_crash_deferred = true;
		g_boot_reset_reason = reason;
	} else if (reset_is_voluntary(reason)) {
		LOG_INF("reset is voluntary — crash ring not updated (empty is expected)");
		if (reason == ESP_RST_UNKNOWN) {
			LOG_INF("reset unknown often means USB/UART reset, not a firmware fault");
		}
	} else {
		LOG_WRN("reset reason not classified as crash — ring unchanged");
	}

	if (crash_ring_pending_count() > 0U) {
		g_has_pending = true;
	}

	LOG_INF("crash ring pending=%u", crash_ring_pending_count());
}

void crash_report_persist_boot_crash(void)
{
	if (!g_boot_crash_deferred) {
		return;
	}

	g_boot_crash_deferred = false;
	persist_boot_crash(g_boot_reset_reason);
	LOG_INF("crash ring pending=%u (post-boot persist)", crash_ring_pending_count());
}

bool crash_report_pending(void)
{
	return crash_ring_pending_count() > 0U || (g_has_pending && g_pending.valid);
}

uint8_t crash_report_pending_count(void)
{
	return crash_ring_pending_count();
}

const struct crash_report_info *crash_report_info(void)
{
	return g_has_pending ? &g_pending : NULL;
}

uint32_t crash_report_dump_size(void)
{
	return g_has_pending ? g_pending.dump_size : 0U;
}

int crash_report_dump_read(off_t offset, uint8_t *buf, size_t len)
{
	struct coredump_cmd_copy_arg copy_arg;

#if !defined(CONFIG_DEBUG_COREDUMP) || !defined(CONFIG_DEBUG_COREDUMP_BACKEND_FLASH_PARTITION)
	ARG_UNUSED(offset);
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
	return -ENOTSUP;
#else
	if (!g_has_pending || buf == NULL || len == 0U) {
		return -EINVAL;
	}

	if ((size_t)offset >= g_pending.dump_size) {
		return 0;
	}

	if ((size_t)offset + len > g_pending.dump_size) {
		len = g_pending.dump_size - (size_t)offset;
	}

	copy_arg.offset = offset;
	copy_arg.buffer = buf;
	copy_arg.length = len;
	return coredump_cmd(COREDUMP_CMD_COPY_STORED_DUMP, &copy_arg);
#endif
}

int crash_report_list_json(char *buf, size_t len)
{
	return crash_ring_list_json(buf, len);
}

int crash_report_info_json(char *buf, size_t len)
{
	const uint8_t pending = crash_ring_pending_count();

	if (buf == NULL || len == 0U) {
		return -EINVAL;
	}

	if (pending == 0U) {
		return snprintf(buf, len, "{\"pending\":0,\"fw\":\"%s\",\"fwc\":%u}",
				FW_VERSION_NAME, (unsigned)FW_VERSION_CODE);
	}

	/* Single-slot detail shortcut only applies while exactly one crash is pending — with 2+
	 * pending, g_active_slot (set at boot by persist_boot_crash()) being pending is not enough
	 * to justify skipping the bulk list, since it would silently hide every other pending
	 * slot from the caller (this previously made the phone/cloud believe only the most
	 * recently-booted crash existed whenever 2+ had accumulated). */
	if (pending == 1U) {
		const int slot = (g_active_slot >= 0 && crash_ring_slot_pending((uint8_t)g_active_slot))
					  ? g_active_slot
					  : crash_ring_first_pending_slot();

		if (slot >= 0) {
			g_active_slot = (int8_t)slot;
			return crash_ring_info_json((uint8_t)slot, buf, len);
		}
	}

	return crash_ring_list_json(buf, len);
}

int crash_report_info_json_slot(uint8_t slot, char *buf, size_t len)
{
	g_active_slot = (int8_t)slot;
	return crash_ring_info_json(slot, buf, len);
}

void crash_report_clear_slot(uint8_t slot)
{
	(void)crash_ring_clear_slot(slot);
	if (g_active_slot == (int8_t)slot) {
		g_active_slot = -1;
	}
	if (crash_ring_pending_count() == 0U) {
		g_has_pending = false;
		memset(&g_pending, 0, sizeof(g_pending));
	}
}

/* Clears several slots in one flash erase+rewrite cycle instead of N — see
 * crash_ring_clear_slots()'s doc comment for why chaining N single-slot clears from a BLE GATT
 * write callback could starve the main-loop stall watchdog and reboot the device mid-clear. */
void crash_report_clear_slots(const uint8_t *slots, size_t n)
{
	(void)crash_ring_clear_slots(slots, n);
	for (size_t i = 0; i < n; i++) {
		if (g_active_slot == (int8_t)slots[i]) {
			g_active_slot = -1;
		}
	}
	if (crash_ring_pending_count() == 0U) {
		g_has_pending = false;
		memset(&g_pending, 0, sizeof(g_pending));
	}
}

void crash_report_clear(void)
{
	crash_ring_clear_all();
	g_has_pending = false;
	g_active_slot = -1;
	memset(&g_pending, 0, sizeof(g_pending));
#if defined(CONFIG_DEBUG_COREDUMP) && defined(CONFIG_DEBUG_COREDUMP_BACKEND_FLASH_PARTITION)
	(void)coredump_cmd(COREDUMP_CMD_INVALIDATE_STORED_DUMP, NULL);
#endif
	LOG_INF("crash reports cleared");
}

const char *crash_report_reset_reason_str(void)
{
	return reset_reason_str((esp_reset_reason_t)g_reset_code);
}

uint8_t crash_report_reset_reason_code(void)
{
	return g_reset_code;
}
