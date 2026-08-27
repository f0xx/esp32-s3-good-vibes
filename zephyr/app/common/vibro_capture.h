#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_config.h"
#include "imu_sample.h"
#include "vibro_features.h"
#include "vibro_band_rms.h"
#include "vibro_ref_store.h"

enum vibro_level {
	VIBRO_LEVEL_OK = 0,
	VIBRO_LEVEL_WARN = 1,
	VIBRO_LEVEL_ALERT = 2,
};

struct vibro_verdict {
	enum vibro_level level;
	float rms_g;
	float peak_g;
	float rms_delta;
	float peak_delta;
	float corr;
	float band_corr;
	float band_delta_max;
	bool valid;
	bool has_reference;
	bool has_band_ref;
};

struct vibro_metrics {
	float rms_g;
	float peak_g;
	uint32_t samples;
	bool valid;
};

#define VIBRO_CAPTURE_MAX_SAMPLES 256

/** profiles.txt #2: "up to 5 ideal sampling profiles of the length of 30s max". */
#define VIBRO_REF_MAX_RECORD_SEC 30.0f

void vibro_capture_init(void);
void vibro_capture_apply_config(const struct device_config_v1 *cfg);
void vibro_capture_reset(void);
void vibro_capture_push(const struct imu_sample *sample);
/** Begin recording a new reference into `slot` (0..VIBRO_REF_STORE_SLOTS-1).
 * `name` may be NULL/empty for an auto-generated "slot N" name. Capped at
 * VIBRO_REF_MAX_RECORD_SEC wall-clock seconds or VIBRO_REF_MAG_MAX samples,
 * whichever comes first. */
bool vibro_capture_start_reference(uint8_t slot, const char *name);
/** Stop recording (or no-op if not recording); flash commit runs on the main thread
 * via vibro_capture_poll(). */
bool vibro_capture_stop_reference(void);
/** Main-loop poll: FFT + flash write for a deferred reference commit. */
void vibro_capture_poll(void);
bool vibro_capture_reference_ready(void);
bool vibro_capture_reference_recording(void);
size_t vibro_capture_reference_len(void);
/** Load a previously-recorded slot as the live/active reference. */
int vibro_capture_select_reference(uint8_t slot);
/** Erase a slot; clears the live reference too if it was the active one. */
int vibro_capture_delete_reference(uint8_t slot);
/** Erase every reference slot and clear live reference RAM state. */
int vibro_capture_clear_all_references(void);
int8_t vibro_capture_active_reference_slot(void);
/** Compact per-slot metadata (name/duration/rms/valid/active) as JSON. */
int vibro_capture_list_references_json(char *buf, size_t len);
struct vibro_metrics vibro_capture_metrics_live(void);
struct vibro_edge_features vibro_capture_edge_features(void);
struct vibro_band_rms vibro_capture_band_rms(void);
struct vibro_verdict vibro_capture_verdict(void);
void vibro_capture_on_status_seq(uint32_t seq, bool persist_flash);
bool vibro_capture_ack_offload(uint32_t seq);
void vibro_capture_session_tick(uint32_t now_ms);
uint32_t vibro_capture_last_ack_seq(void);
uint32_t vibro_capture_pending_session_seq(void);
uint16_t vibro_capture_pending_offload_count(void);
