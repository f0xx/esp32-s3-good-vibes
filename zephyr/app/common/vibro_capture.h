#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_config.h"
#include "imu_sample.h"
#include "vibro_features.h"
#include "vibro_band_rms.h"

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

void vibro_capture_init(void);
void vibro_capture_apply_config(const struct device_config_v1 *cfg);
void vibro_capture_reset(void);
void vibro_capture_push(const struct imu_sample *sample);
bool vibro_capture_start_reference(void);
bool vibro_capture_stop_reference(void);
bool vibro_capture_reference_ready(void);
bool vibro_capture_refresh_ref_bands(void);
size_t vibro_capture_reference_len(void);
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
