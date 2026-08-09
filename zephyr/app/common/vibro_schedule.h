#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "device_config.h"

enum vibro_schedule_mode {
	VIBRO_SCHEDULE_ALWAYS = 0,
	VIBRO_SCHEDULE_INTERVAL = 1,
	VIBRO_SCHEDULE_RANDOM = 2,
};

#define VIBRO_PRE_CAPTURE_PAUSE_SEC 5U

uint32_t vibro_schedule_effective_window_sec(const struct device_config_v1 *cfg, uint32_t bucket);
bool vibro_schedule_capture_active(const struct device_config_v1 *cfg, uint32_t now_ms);
bool vibro_schedule_capture_prep_active(const struct device_config_v1 *cfg, uint32_t now_ms);
void vibro_schedule_window_info(const struct device_config_v1 *cfg, uint32_t now_ms, bool *active,
				uint32_t *sec_left, uint32_t *sec_until);
