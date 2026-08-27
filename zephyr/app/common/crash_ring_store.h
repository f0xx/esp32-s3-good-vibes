#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "crash_report.h"

#define CRASH_RING_SLOTS 5U

struct crash_ring_telemetry {
	uint8_t render_hz;
	uint8_t imu_hz;
	uint16_t bat_mv;
	uint8_t bat_pct;
	uint8_t power_profile;
};

int crash_ring_init(void);
int crash_ring_append(const struct crash_report_info *info, const struct crash_ring_telemetry *tel);
int crash_ring_append_soft(const struct crash_report_info *info, const struct crash_ring_telemetry *tel);
uint8_t crash_ring_pending_count(void);
bool crash_ring_slot_valid(uint8_t slot);
bool crash_ring_slot_pending(uint8_t slot);
int crash_ring_list_json(char *buf, size_t len);
int crash_ring_info_json(uint8_t slot, char *buf, size_t len);
/** These no longer touch flash synchronously — they only flip a RAM-only "logically cleared"
 * bit, visible immediately to crash_ring_pending_count()/crash_ring_slot_pending()/
 * crash_ring_list_json(). See crash_ring_flush_ram_clears()'s doc comment for why the actual
 * flash write is deferred, and for what happens if power is lost before it runs (nothing
 * unsafe — worst case the slot re-reports pending next boot and gets re-uploaded, which the
 * backend already dedupes). */
int crash_ring_clear_slot(uint8_t slot);
int crash_ring_clear_slots(const uint8_t *slots, size_t n);
void crash_ring_clear_all(void);
int crash_ring_first_pending_slot(void);
int crash_ring_next_pending_slot(uint8_t after);

/**
 * Persists any RAM-only "cleared" slots (see above) to flash as a single ping-pong cycle, if
 * any are pending. Callers MUST only invoke this while no BLE connection is active — real
 * hardware testing showed a live connection makes flash_area_erase()'s interrupts/cache-disabled
 * window unsafe at every offset from connect tried so far (not just near the connection
 * setup/grace-elapse boundaries), silently taking down both cores with TG0WDT_SYS_RST and no
 * preceding fatal-exception log. No-op (and safe to call unconditionally) if nothing is pending.
 */
void crash_ring_flush_ram_clears(void);
