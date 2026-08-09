#pragma once

#include <stdbool.h>

#include "attitude.h"
#include "imu_cal.h"
#include "walk_distance.h"

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
bool imu_pipeline_live(void);
bool imu_pipeline_ready(void);
bool imu_pipeline_recovering(void);
void imu_pipeline_request_recover(void);
