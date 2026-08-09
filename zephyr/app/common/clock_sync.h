#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Apply phone correction when |drift| exceeds this (4 min; MQTT window is ~5 min). */
#define CLOCK_SYNC_DRIFT_APPLY_MS 240000LL

enum clock_sync_source {
	CLOCK_SYNC_SRC_NONE = 0,
	CLOCK_SYNC_SRC_PHONE = 1,
	CLOCK_SYNC_SRC_NTP = 2,
};

void clock_sync_begin(void);

/** Phone/BLE write — applies wall clock only if unsynced or drift > threshold; always stores tz. */
bool clock_sync_set_from_phone(int64_t unix_ms, int16_t tz_offset_min);

/** SNTP result (UTC). Resets the 5-day NTP schedule. */
bool clock_sync_set_from_ntp(int64_t unix_ms);

bool clock_sync_is_synced(void);
enum clock_sync_source clock_sync_source(void);
int16_t clock_sync_tz_offset_min(void);

/** Signed drift (reference_unix_ms - local_now_ms) at last phone check. */
int64_t clock_sync_last_drift_ms(void);

/** Magnitude of last applied correction (ms), 0 if none yet. */
int64_t clock_sync_last_corr_ms(void);

int64_t clock_sync_now_ms(void);
uint32_t clock_sync_now_ms32(void);
uint32_t clock_sync_now_unix_sec(void);

/** Drift vs reference Unix ms (positive = local clock behind). */
int64_t clock_sync_drift_ms(int64_t reference_unix_ms);

/** Append compact clock fields to STATUS JSON (requires room in buffer). */
int clock_sync_append_status_json(char *buf, size_t cap, int written);

void clock_sync_ntp_init(void);
void clock_sync_ntp_on_wifi_connected(void);
void clock_sync_ntp_reset_schedule(void);
void clock_sync_ntp_poll(void);
