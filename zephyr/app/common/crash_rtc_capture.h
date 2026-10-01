/*
 * Captures CPU-exception detail (exccause/excvaddr/faulting-thread) into RTC "no-init" SRAM
 * from inside Zephyr's fatal-error path, so it survives the reboot that follows and can be
 * folded into the crash_ring_store record on the very next boot.
 *
 * Why this exists: flash-partition coredump is unsafe on ESP32-S3 (write from the fault
 * handler with cache disabled → fault storm / TG0WDT). Logging-backend coredump is also
 * unsafe here: with Invalid SP it never returns, so the fatal handler never runs and the
 * board stays online-dead until USB reset. Desk/release builds therefore leave
 * CONFIG_DEBUG_COREDUMP off; this module is the sole post-mortem breadcrumb for genuine
 * Zephyr-caught exceptions (z_fatal_error() → k_sys_fatal_error_handler()).
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

struct arch_esf;

#define CRASH_RTC_BACKTRACE_MAX 8U

struct crash_rtc_capture {
	uint32_t pc;        /* EPC1 / exception PC — the actual fault address */
	uint32_t a0;        /* return address at the fault */
	uint32_t sp;
	uint32_t exccause;
	uint32_t excvaddr;
	uint32_t backtrace[CRASH_RTC_BACKTRACE_MAX];
	uint8_t bt_count;
	uint8_t reason;     /* Zephyr K_ERR_* code */
	char thread_name[16];
};

/** Called from our k_sys_fatal_error_handler() override — see crash_rtc_capture.c. Plain SRAM
 * writes only, no flash access, safe to call unconditionally from the fault path. */
void crash_rtc_capture_on_fatal(unsigned int reason, const char *thread_name,
				const struct arch_esf *esf);

/**
 * Soft task-WDT path: record which feeder channel starved (main/render) *before* reboot.
 * Do not call k_panic() after this — that overwrites PC with coredump/logging frames.
 * Uses a private reason code so boot can still label the ring record as task_wdt.
 */
#define CRASH_RTC_REASON_TASK_WDT 0xF0U
void crash_rtc_capture_task_wdt(const char *channel_name);

/** True if a valid, not-yet-consumed capture is sitting in RTC (does not consume). */
bool crash_rtc_capture_pending(void);

/** Call once on boot, before persist_boot_crash() decides what to put in the crash ring.
 * Returns true and fills `out` if a valid, not-yet-consumed capture exists (i.e. the previous
 * boot ended in a captured Zephyr fatal error); always clears the RTC copy so it isn't
 * replayed into a later, unrelated boot's crash record. */
bool crash_rtc_capture_consume(struct crash_rtc_capture *out);
