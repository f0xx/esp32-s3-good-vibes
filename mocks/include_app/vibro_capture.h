#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_config.h"
#include "imu_sample.h"

/* Thin host stand-in — real vibro_capture.h pulls FFT/ref-store graph. */

enum vibro_level {
	VIBRO_LEVEL_OK = 0,
	VIBRO_LEVEL_WARN = 1,
	VIBRO_LEVEL_ALERT = 2,
};

struct vibro_verdict {
	enum vibro_level level;
	float rms_g;
	bool valid;
};

void vibro_capture_init(void);
void vibro_capture_apply_config(const struct device_config_v1 *cfg);
void vibro_capture_push(const struct imu_sample *sample);
