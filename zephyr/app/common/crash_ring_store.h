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
uint8_t crash_ring_pending_count(void);
bool crash_ring_slot_valid(uint8_t slot);
bool crash_ring_slot_pending(uint8_t slot);
int crash_ring_list_json(char *buf, size_t len);
int crash_ring_info_json(uint8_t slot, char *buf, size_t len);
int crash_ring_clear_slot(uint8_t slot);
/** Clears several slots' pending flags in a single flash erase+rewrite cycle. */
int crash_ring_clear_slots(const uint8_t *slots, size_t n);
void crash_ring_clear_all(void);
int crash_ring_first_pending_slot(void);
int crash_ring_next_pending_slot(uint8_t after);
