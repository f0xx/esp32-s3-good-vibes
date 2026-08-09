#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "attitude.h"
#include "imu_sample.h"

enum walk_surface {
	WALK_SURFACE_FLAT_HARD = 0,
	WALK_SURFACE_FLAT_SOFT,
	WALK_SURFACE_UNEVEN,
};

struct walk_distance_state {
	uint32_t steps;
	float distance_m;
	float last_step_m;
	float cadence_hz;
	float step_signal;
	float peak_g;
	bool walking;
	bool counting;
	enum walk_surface surface;
};

struct walk_distance_config {
	float user_height_m;
	float pocket_height_m;
	float step_min_m;
	float step_max_m;
};

struct walk_distance_estimator {
	struct walk_distance_config cfg;
	struct walk_distance_state state;
	float mag_slow_ema;
	float mag_hp_lp;
	float vert_base_ema;
	float vert_hp_lp;
	float vert_slow_ema;
	uint16_t elevator_hold;
	bool armed;
	float peak_armed;
	uint32_t armed_since_ms;
	uint32_t last_step_ms;
	uint32_t last_cadence_ms;
	uint8_t cadence_window_steps;
};

void walk_distance_init(struct walk_distance_estimator *est,
			const struct walk_distance_config *cfg);
void walk_distance_reset(struct walk_distance_estimator *est);
void walk_distance_set_config(struct walk_distance_estimator *est,
			      const struct walk_distance_config *cfg);
void walk_distance_set_surface(struct walk_distance_estimator *est, enum walk_surface surface);
void walk_distance_update(struct walk_distance_estimator *est, const struct imu_sample *sample,
			  const struct attitude_state *att, float dt_sec, uint32_t now_ms);
const struct walk_distance_state *walk_distance_state_get(
	const struct walk_distance_estimator *est);
