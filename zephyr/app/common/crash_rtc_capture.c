#include "crash_rtc_capture.h"

#include <stdio.h>
#include <string.h>

#include <zephyr/arch/cpu.h>
#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#define CRASH_RTC_MAGIC 0x43524352U /* CRCR */

struct crash_rtc_slot {
	uint32_t magic;
	struct crash_rtc_capture cap;
	uint32_t crc32;
};

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

static bool slot_valid(void)
{
	return g_rtc_slot.magic == CRASH_RTC_MAGIC && g_rtc_slot.crc32 == slot_crc(&g_rtc_slot);
}

/* ESP32-S3 IRAM (0x4037xxxx) + IROM/flash (0x42000000). Dummy arch_esf walks
 * otherwise yield 0 / DRAM / IllegalInstruction-looking junk. */
#define CRASH_S3_CODE_LO 0x3F000000U
#define CRASH_S3_CODE_HI 0x50000000U

static uint32_t sanitize_xtensa_pc(uint32_t pc)
{
	/* Windowed ABI: top two bits of a0 are the window increment, not address. */
	if ((pc & 0x80000000U) != 0U) {
		pc = (pc & 0x3fffffffU) | 0x40000000U;
	}
	return pc;
}

static bool pc_in_s3_code(uint32_t pc)
{
	return pc >= CRASH_S3_CODE_LO && pc < CRASH_S3_CODE_HI;
}

static bool sp_in_s3_ram(uint32_t sp)
{
	/* ESP32-S3 internal DRAM. Spill slots must be 4-byte aligned. */
	return (sp % 4U) == 0U && sp >= 0x3FC00000U && sp < 0x3FD00000U;
}

static void store_bt_pc(struct crash_rtc_capture *cap, uint32_t pc)
{
	pc = sanitize_xtensa_pc(pc);
	if (!pc_in_s3_code(pc) || cap->bt_count >= CRASH_RTC_BACKTRACE_MAX) {
		return;
	}
	for (uint8_t j = 0; j < cap->bt_count; j++) {
		if (cap->backtrace[j] == pc) {
			return;
		}
	}
	cap->backtrace[cap->bt_count++] = pc;
}

/* Windowed ABI: 4-word caller spill under SP (a0 at SP-16, a1 at SP-12).
 * Dummy arch_esf walks are rejected by the code/RAM range checks so we never
 * persist 0 / DRAM / IllegalInstruction-looking junk. */
static void walk_windowed_backtrace(struct crash_rtc_capture *cap, uint32_t sp)
{
	for (uint8_t i = 0; i < CRASH_RTC_BACKTRACE_MAX; i++) {
		if (!sp_in_s3_ram(sp) || !sp_in_s3_ram(sp - 16U)) {
			break;
		}

		uint32_t next_pc = *(volatile uint32_t *)(uintptr_t)(sp - 16U);
		uint32_t next_sp = *(volatile uint32_t *)(uintptr_t)(sp - 12U);

		store_bt_pc(cap, next_pc);
		if (next_sp <= sp || !sp_in_s3_ram(next_sp)) {
			break;
		}
		sp = next_sp;
	}
}

static void capture_xtensa_regs(struct crash_rtc_capture *cap, const struct arch_esf *esf)
{
#if defined(CONFIG_XTENSA)
	uint32_t epc1 = 0U;
	uint32_t depc = 0U;
	uint32_t a0 = 0U;
	uint32_t sp = 0U;
	uint32_t walk_sp;

	__asm__ volatile("rsr.epc1 %0" : "=r"(epc1));
	__asm__ volatile("rsr.depc %0" : "=r"(depc));
	__asm__ volatile("rsr.exccause %0" : "=r"(cap->exccause));
	__asm__ volatile("rsr.excvaddr %0" : "=r"(cap->excvaddr));
	__asm__ volatile("mov %0, a0" : "=r"(a0));
	__asm__ volatile("mov %0, a1" : "=r"(sp));

	/* EPC1 is the instruction that faulted. DEPC is the nested/double-exception PC.
	 * Prefer a non-zero EPC1; fall back to DEPC (cache-disabled flash fetch storms
	 * often nest). Zephyr's arch_esf is a dummy int on Xtensa — do not treat it as
	 * a register file. */
	cap->pc = epc1 != 0U ? epc1 : depc;
	cap->a0 = a0;
	cap->sp = sp;

	/* Real entry point first, even if the stack walk later yields nothing. */
	if (cap->pc != 0U) {
		cap->backtrace[0] = cap->pc;
		cap->bt_count = 1U;
	}
	store_bt_pc(cap, a0);

	walk_sp = sp;
	if (esf != NULL && sp_in_s3_ram((uint32_t)(uintptr_t)esf)) {
		walk_sp = (uint32_t)(uintptr_t)esf;
	}
	walk_windowed_backtrace(cap, walk_sp);
#else
	ARG_UNUSED(esf);
	ARG_UNUSED(cap);
#endif
}

void crash_rtc_capture_on_fatal(unsigned int reason, const char *thread_name,
				const struct arch_esf *esf)
{
	memset(&g_rtc_slot.cap, 0, sizeof(g_rtc_slot.cap));
	g_rtc_slot.cap.reason = (uint8_t)MIN(reason, 0xFFU);
	capture_xtensa_regs(&g_rtc_slot.cap, esf);

	snprintf(g_rtc_slot.cap.thread_name, sizeof(g_rtc_slot.cap.thread_name), "%s",
		 (thread_name != NULL && thread_name[0] != '\0') ? thread_name : "unknown");

	g_rtc_slot.crc32 = slot_crc(&g_rtc_slot);
	g_rtc_slot.magic = CRASH_RTC_MAGIC;
}

void crash_rtc_capture_task_wdt(const char *channel_name)
{
	uint32_t a0 = 0U;
	uint32_t sp = 0U;

	memset(&g_rtc_slot.cap, 0, sizeof(g_rtc_slot.cap));
	g_rtc_slot.cap.reason = (uint8_t)CRASH_RTC_REASON_TASK_WDT;
	g_rtc_slot.cap.exccause = 0U;

#if defined(CONFIG_XTENSA)
	__asm__ volatile("mov %0, a0" : "=r"(a0));
	__asm__ volatile("mov %0, a1" : "=r"(sp));
	g_rtc_slot.cap.a0 = a0;
	g_rtc_slot.cap.sp = sp;
	/* Prefer return address as a breadcrumb; not the starved thread's PC, but better than
	 * letting k_panic()/coredump overwrite the slot with logging-backend frames. */
	store_bt_pc(&g_rtc_slot.cap, a0);
	walk_windowed_backtrace(&g_rtc_slot.cap, sp);
	if (g_rtc_slot.cap.bt_count > 0U) {
		g_rtc_slot.cap.pc = g_rtc_slot.cap.backtrace[0];
	}
#endif

	snprintf(g_rtc_slot.cap.thread_name, sizeof(g_rtc_slot.cap.thread_name), "%s",
		 (channel_name != NULL && channel_name[0] != '\0') ? channel_name : "wdt");

	g_rtc_slot.crc32 = slot_crc(&g_rtc_slot);
	g_rtc_slot.magic = CRASH_RTC_MAGIC;
}

bool crash_rtc_capture_pending(void)
{
	return slot_valid();
}

bool crash_rtc_capture_consume(struct crash_rtc_capture *out)
{
	bool valid = slot_valid();

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
 * after z_fatal_error()'s printk of the exception header (coredump is disabled — logging
 * backend never returned on Invalid SP). Only plain SRAM writes here, no flash / no USB.
 *
 * Always reboot immediately: LOG_PANIC()/USB CDC flush can wedge the same way coredump did,
 * and with CONFIG_TASK_WDT_HW_FALLBACK=n nothing else will reset us. Soft WDT cannot run here.
 */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	const char *thread_name =
		IS_ENABLED(CONFIG_MULTITHREADING) ? k_thread_name_get(k_current_get()) : NULL;

	crash_rtc_capture_on_fatal(reason, thread_name, esf);
	sys_reboot(SYS_REBOOT_COLD);
	CODE_UNREACHABLE;
}
