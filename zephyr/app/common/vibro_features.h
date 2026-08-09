#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "imu_sample.h"

struct vibro_edge_features {
	float crest;
	float zcr_hz;
	float hf_ratio;
	bool valid;
};

struct vibro_edge_features vibro_features_compute(const struct imu_sample *samples, size_t count,
						  float sample_hz);

typedef float (*vibro_mag_at_fn)(void *ctx, size_t index);

struct vibro_edge_features vibro_features_compute_series(size_t count, float sample_hz,
							 vibro_mag_at_fn mag_at, void *ctx);
