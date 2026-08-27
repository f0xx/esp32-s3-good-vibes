/*
 * Captures CPU-exception detail (exccause/excvaddr/faulting-thread) into RTC "no-init" SRAM
 * from inside Zephyr's fatal-error path, so it survives the reboot that follows and can be
 * folded into the crash_ring_store record on the very next boot.
 *
 * Why this exists: prj_crash.conf intentionally uses CONFIG_DEBUG_COREDUMP_BACKEND_LOGGING
 * instead of ...BACKEND_FLASH_PARTITION (see that file's comment — the flash backend writes
 * from inside the fault handler with cache disabled, which caused a real fault-storm/
 * TG0WDT_SYS_RST on this hardware). The logging backend is safe but only printk()s the
 * register dump — if nobody is watching the live UART at that exact moment, that detail is
 * gone forever once the board reboots, and crash_ring_store only ends up with a bare
 * reset_reason ("wdt", "panic", ...) with pc/exccause/excvaddr all zero. This module closes
 * that gap for genuine Zephyr-caught CPU exceptions/panics/asserts (K_ERR_CPU_EXCEPTION and
 * friends, routed through z_fatal_error() -> k_sys_fatal_error_handler()).
 *
 * Deliberate non-goal: this does NOT (and structurally cannot) capture anything for a
 * TG0WDT_SYS_RST caused by CONFIG_TASK_WDT_HW_FALLBACK — that path is a pure hardware timer
 * resetting the SoC because software never ran at all (e.g. stuck with cache disabled during
 * a flash op), so there is no CPU exception, no esf, no handler invocation to hook. That
 * specific hazard is what the flash_safety.h work (see crash_ring_store.c et al.) prevents at
 * the source instead.
 *
 * RTC "no-init" SRAM is powered by the RTC domain, so it survives any reset that keeps power
 * applied (software reset, panic, both watchdog paths) but is naturally garbage on a true
 * power-on — hence the magic+CRC validation before trusting its contents.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct crash_rtc_capture {
	uint32_t exccause;
	uint32_t excvaddr;
	uint8_t reason;     /* Zephyr K_ERR_* code */
	char thread_name[16];
};

/** Called from our k_sys_fatal_error_handler() override — see crash_rtc_capture.c. Plain SRAM
 * writes only, no flash access, safe to call unconditionally from the fault path. */
void crash_rtc_capture_on_fatal(unsigned int reason, const char *thread_name);

/** Call once on boot, before persist_boot_crash() decides what to put in the crash ring.
 * Returns true and fills `out` if a valid, not-yet-consumed capture exists (i.e. the previous
 * boot ended in a captured Zephyr fatal error); always clears the RTC copy so it isn't
 * replayed into a later, unrelated boot's crash record. */
bool crash_rtc_capture_consume(struct crash_rtc_capture *out);
