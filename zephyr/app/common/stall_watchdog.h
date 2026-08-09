#pragma once

#include <stdbool.h>
#include <stdint.h>

/** Step 3a: task WDT + render stall → panic (coredump + reboot). */
int stall_watchdog_init(void);
/** Call once from the render thread entry (WDT not armed during boot). */
void stall_watchdog_arm_render(void);
void stall_watchdog_feed_main(void);
void stall_watchdog_feed_render(void);
/** Called each ~10 s telemetry window from main. */
void stall_watchdog_hb_window(uint32_t render_frames, bool screen_on, uint32_t imu_ticks);
