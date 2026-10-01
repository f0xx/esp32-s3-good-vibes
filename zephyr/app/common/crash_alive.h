/*
 * RTC "alive" heartbeat — detects silent hangs that never reach Zephyr FATAL.
 *
 * Soft task WDT cannot run when IRQs/cache are dead; HW fallback is off on
 * purpose. Desk recovery is often USB reset (reason 11) with empty RTC fatal
 * capture → no crash_ring → nothing in the cloud. This module marks "we were
 * running" in RTC; intentional reboots clear it. Next boot with dirty alive
 * + USB/WDT/other → soft ring row "silent_hang".
 *
 * Also stamps main_step / render_stage and a 1 Hz timer tick so the next
 * silent_hang log can tell "main stuck, timers alive" vs "whole OS frozen".
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void crash_alive_init(void);

void crash_alive_mark_running(uint32_t uptime_ms, const char *main_step, uint8_t render_stage);

void crash_alive_mark_clean_shutdown(void);

/** True if previous boot left a dirty alive stamp (likely silent hang/reset). */
bool crash_alive_dirty(void);

/** Uptime last stamped before the unexplained reset (0 if none). */
uint32_t crash_alive_last_uptime_ms(void);

/** 1 Hz timer ticks last stamped (0 if none). */
uint32_t crash_alive_last_timer_ticks(void);

/** Copy last main_step (NUL-terminated). */
void crash_alive_last_step(char *out, size_t out_len);

uint8_t crash_alive_last_render_stage(void);

/** Consume dirty state (clears RTC slot). */
void crash_alive_consume(void);
