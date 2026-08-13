#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "attitude.h"
#include "imu_cal.h"
#include "imu_sample.h"
#include "walk_distance.h"

/* Sized well above the worst-case sample/BLE-tick ratio (100 Hz IMU / ~30 Hz BLE tick ≈ 3-4
 * samples per drain) so a slightly late BLE tick still gets the full backlog in one notify. */
#define IMU_PIPELINE_RAW_RING_CAP 16

struct imu_pipeline_raw_entry {
	uint32_t t_ms;
	struct imu_sample sample;
};

bool imu_pipeline_init(void);
void imu_pipeline_poll(void);
void imu_pipeline_reschedule(void);
uint32_t imu_pipeline_take_hb_ticks(void);
bool imu_pipeline_read_raw(struct imu_sample *out);
/** Thread-safe copy for render/BLE (preferred over latest/attitude pointers). */
bool imu_pipeline_snapshot(struct imu_sample *sample, struct attitude_estimator *att);
const struct imu_sample *imu_pipeline_latest(void);
const struct attitude_estimator *imu_pipeline_attitude(void);
const struct walk_distance_state *imu_pipeline_walk_state(void);
float imu_pipeline_walk_distance_m(void);
void imu_pipeline_apply_config(void);
bool imu_pipeline_tick(float dt_sec);
/** Drains up to `max` raw samples buffered since the last drain (oldest first), for BLE
 *  RAW-mode batching (ble_imu_gatt.c build_batch()) — lets one notify carry several IMU
 *  samples instead of only the single latest one. Non-RAW BLE modes never call this, so the
 *  ring just keeps overwriting its oldest entries (harmless, no leak/growth). */
size_t imu_pipeline_drain_raw(struct imu_pipeline_raw_entry *out, size_t max);
bool imu_pipeline_live(void);
bool imu_pipeline_ready(void);
bool imu_pipeline_recovering(void);
void imu_pipeline_request_recover(void);
