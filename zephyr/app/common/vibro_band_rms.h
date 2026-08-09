#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "vibro_features.h"

#define VIBRO_BAND_COUNT 4U
#define VIBRO_BAND_FFT_SIZE 64U

struct vibro_band_rms {
	float bands[VIBRO_BAND_COUNT];
	bool valid;
};

struct vibro_band_rms vibro_band_rms_compute_series(size_t count, float sample_hz,
						    vibro_mag_at_fn mag_at, void *ctx);
