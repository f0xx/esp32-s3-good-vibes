#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define CRASH_REPORT_BACKTRACE_MAX 8U

struct crash_report_info {
	bool valid;
	uint32_t seq;
	uint32_t dump_size;
	uint32_t pc;
	uint32_t exccause;
	uint32_t excvaddr;
	uint32_t uptime_ms;
	uint8_t reset_reason;
	const char *reason;
	char fw_version[32];
	uint32_t backtrace[CRASH_REPORT_BACKTRACE_MAX];
	uint8_t backtrace_count;
};

void crash_report_init(void);
/** Append boot crash to flash ring — call after IMU/BIST, before BLE. */
void crash_report_persist_boot_crash(void);
bool crash_report_pending(void);
uint8_t crash_report_pending_count(void);
const struct crash_report_info *crash_report_info(void);
uint32_t crash_report_dump_size(void);
int crash_report_dump_read(off_t offset, uint8_t *buf, size_t len);
int crash_report_list_json(char *buf, size_t len);
int crash_report_info_json(char *buf, size_t len);
int crash_report_info_json_slot(uint8_t slot, char *buf, size_t len);
void crash_report_clear_slot(uint8_t slot);
/** Clears several slots' pending flags in a single flash erase+rewrite cycle. */
void crash_report_clear_slots(const uint8_t *slots, size_t n);
void crash_report_clear(void);
const char *crash_report_reset_reason_str(void);
uint8_t crash_report_reset_reason_code(void);
