#include "crash_rtc_capture.h"

#include <stdio.h>
#include <string.h>

#include <zephyr/arch/cpu.h>
#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(crash_rtc, LOG_LEVEL_INF);

#define CRASH_RTC_MAGIC 0x43524352U /* CRCR */

struct crash_rtc_slot {
	uint32_t magic;
	struct crash_rtc_capture cap;
	uint32_t crc32;
} __packed;

/*
 * RTC "no-init" SRAM — survives any reset that keeps the RTC power domain up (see
 * crash_rtc_capture.h doc comment); garbage on a true cold power-on, hence the CRC check.
 *
 * Placed directly via the raw section name (matching what esp_attr.h's RTC_NOINIT_ATTR would
 * emit: __attribute__((section(".rtc_noinit.N")))) instead of including esp_attr.h, because
 * that header's own macro is gated on `#if CONFIG_SOC_RTC_FAST_MEM_SUPPORTED ||
 * CONFIG_SOC_RTC_SLOW_MEM_SUPPORTED`, and neither CONFIG_ symbol is defined in this Zephyr
 * port's generated sdkconfig.h (only the unprefixed SOC_RTC_*_SUPPORTED from soc_caps.h is) —
 * so RTC_NOINIT_ATTR would silently expand to nothing here and this would end up plain .bss,
 * zeroed every boot, defeating the whole point. The esp32s3 SoC linker script
 * (soc/espressif/esp32s3/default.ld) does define the matching `.rtc_noinit` output section in
 * `rtc_slow_seg`, independent of that Kconfig gate, so the section itself is real.
 */
static struct crash_rtc_slot g_rtc_slot __attribute__((section(".rtc_noinit.crash_capture")));

static uint32_t slot_crc(const struct crash_rtc_slot *s)
{
	return crc32_ieee((const uint8_t *)&s->cap, sizeof(s->cap));
}

void crash_rtc_capture_on_fatal(unsigned int reason, const char *thread_name)
{
	memset(&g_rtc_slot.cap, 0, sizeof(g_rtc_slot.cap));
	g_rtc_slot.cap.reason = (uint8_t)MIN(reason, 0xFFU);

	/* EXCCAUSE/EXCVADDR are only meaningful for an actual CPU exception — for a kernel
	 * panic/oops/stack-check-fail (software-detected, no fresh HW exception necessarily
	 * pending) they'd just be whatever the last unrelated exception left behind, which is
	 * misleading, so leave them 0 for those reasons instead of guessing. */
	if (reason == K_ERR_CPU_EXCEPTION) {
#if defined(CONFIG_XTENSA)
		__asm__ volatile("rsr.exccause %0" : "=r"(g_rtc_slot.cap.exccause));
		__asm__ volatile("rsr.excvaddr %0" : "=r"(g_rtc_slot.cap.excvaddr));
#endif
	}

	snprintf(g_rtc_slot.cap.thread_name, sizeof(g_rtc_slot.cap.thread_name), "%s",
		 (thread_name != NULL && thread_name[0] != '\0') ? thread_name : "unknown");

	g_rtc_slot.crc32 = slot_crc(&g_rtc_slot);
	g_rtc_slot.magic = CRASH_RTC_MAGIC;
}

bool crash_rtc_capture_consume(struct crash_rtc_capture *out)
{
	bool valid = g_rtc_slot.magic == CRASH_RTC_MAGIC && g_rtc_slot.crc32 == slot_crc(&g_rtc_slot);

	if (valid && out != NULL) {
		*out = g_rtc_slot.cap;
	}
	/* Consume-once regardless of validity, so corrupt/stale contents from an unrelated
	 * power-on don't linger and get misread by some future boot. */
	memset(&g_rtc_slot, 0, sizeof(g_rtc_slot));
	return valid;
}

/*
 * Overrides kernel/fatal.c's weak default. Runs on the faulting CPU with interrupts locked,
 * right after coredump()'s (safe, logging-only) register dump — see z_fatal_error(). Only
 * plain SRAM writes here, no flash access, so this is safe to run unconditionally regardless
 * of what caused the fault (unlike the coredump-to-flash backend this app deliberately
 * avoids — see prj_crash.conf).
 */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);

	const char *thread_name =
		IS_ENABLED(CONFIG_MULTITHREADING) ? k_thread_name_get(k_current_get()) : NULL;

	crash_rtc_capture_on_fatal(reason, thread_name);

	LOG_PANIC();
	LOG_ERR("Halting system");
	k_fatal_halt(reason);
	CODE_UNREACHABLE;
}
