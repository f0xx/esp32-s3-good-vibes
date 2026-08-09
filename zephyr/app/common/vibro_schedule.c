#include "vibro_schedule.h"

static uint32_t hash32(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

uint32_t vibro_schedule_effective_window_sec(const struct device_config_v1 *cfg, uint32_t bucket)
{
	uint32_t window = cfg->vibro_window_sec > 0U ? cfg->vibro_window_sec : 10U;
	const uint8_t mix_every = cfg->reserved[0];
	const uint8_t mix_ratio = cfg->reserved[1];

	if (mix_every < 2U || mix_ratio < 2U) {
		return window;
	}
	if ((bucket % mix_every) == (mix_every - 1U)) {
		return window;
	}

	window /= mix_ratio;

	{
		const uint8_t dyn_short = cfg->reserved[2];

		if (dyn_short >= 2U && (bucket % 2U) == 0U) {
			window /= dyn_short;
		}
	}

	{
		const uint8_t dyn_nested = cfg->reserved[3];

		if (dyn_nested >= 2U && (bucket % 4U) == 0U) {
			window /= dyn_nested;
		}
	}

	if (window < 2U) {
		window = 2U;
	}
	return window;
}

bool vibro_schedule_capture_active(const struct device_config_v1 *cfg, uint32_t now_ms)
{
	if (cfg == NULL || cfg->vibro_schedule_mode == VIBRO_SCHEDULE_ALWAYS) {
		return true;
	}

	const uint32_t now_sec = now_ms / 1000U;
	const uint32_t interval = cfg->vibro_interval_sec > 0U ? cfg->vibro_interval_sec : 60U;
	const uint32_t bucket = now_sec / interval;
	uint32_t window = vibro_schedule_effective_window_sec(cfg, bucket);
	uint32_t max_window = cfg->vibro_window_sec > 0U ? cfg->vibro_window_sec : 10U;

	if (window > interval) {
		window = interval;
	}
	if (max_window > interval) {
		max_window = interval;
	}
	if (window == 0U) {
		return false;
	}

	const uint32_t phase = now_sec % interval;
	if (cfg->vibro_schedule_mode == VIBRO_SCHEDULE_INTERVAL) {
		return phase < window;
	}

	const uint32_t span = interval - max_window + 1U;
	const uint32_t slot = hash32(bucket ^ cfg->vibro_jitter_sec) % span;

	return phase >= slot && phase < slot + window;
}

bool vibro_schedule_capture_prep_active(const struct device_config_v1 *cfg, uint32_t now_ms)
{
	uint32_t sec_until = 0U;

	if (cfg == NULL || cfg->vibro_schedule_mode == VIBRO_SCHEDULE_ALWAYS) {
		return false;
	}

	if (vibro_schedule_capture_active(cfg, now_ms)) {
		return false;
	}

	vibro_schedule_window_info(cfg, now_ms, NULL, NULL, &sec_until);
	return sec_until > 0U && sec_until <= VIBRO_PRE_CAPTURE_PAUSE_SEC;
}

static void window_bounds(const struct device_config_v1 *cfg, uint32_t bucket, uint32_t *start_out,
			    uint32_t *end_out)
{
	const uint32_t interval = cfg->vibro_interval_sec > 0U ? cfg->vibro_interval_sec : 60U;
	uint32_t window = vibro_schedule_effective_window_sec(cfg, bucket);
	uint32_t max_window = cfg->vibro_window_sec > 0U ? cfg->vibro_window_sec : 10U;

	if (window > interval) {
		window = interval;
	}
	if (max_window > interval) {
		max_window = interval;
	}

	if (cfg->vibro_schedule_mode == VIBRO_SCHEDULE_INTERVAL) {
		*start_out = 0U;
		*end_out = window;
		return;
	}

	const uint32_t span = interval - max_window + 1U;
	const uint32_t slot = hash32(bucket ^ cfg->vibro_jitter_sec) % span;

	*start_out = slot;
	*end_out = slot + window;
}

void vibro_schedule_window_info(const struct device_config_v1 *cfg, uint32_t now_ms, bool *active,
				uint32_t *sec_left, uint32_t *sec_until)
{
	if (active != NULL) {
		*active = false;
	}
	if (sec_left != NULL) {
		*sec_left = 0U;
	}
	if (sec_until != NULL) {
		*sec_until = 0U;
	}

	if (cfg == NULL || cfg->vibro_schedule_mode == VIBRO_SCHEDULE_ALWAYS) {
		if (active != NULL) {
			*active = true;
		}
		if (sec_left != NULL) {
			*sec_left = 3600U;
		}
		return;
	}

	const uint32_t now_sec = now_ms / 1000U;
	const uint32_t interval = cfg->vibro_interval_sec > 0U ? cfg->vibro_interval_sec : 60U;
	const uint32_t bucket = now_sec / interval;
	uint32_t window = vibro_schedule_effective_window_sec(cfg, bucket);

	if (window > interval) {
		window = interval;
	}
	if (window == 0U) {
		return;
	}

	const uint32_t phase = now_sec % interval;
	uint32_t start = 0U;
	uint32_t end = window;

	window_bounds(cfg, bucket, &start, &end);

	if (phase >= start && phase < end) {
		if (active != NULL) {
			*active = true;
		}
		if (sec_left != NULL) {
			*sec_left = end - phase;
		}
		return;
	}

	if (sec_until == NULL) {
		return;
	}

	if (phase < start) {
		*sec_until = start - phase;
		return;
	}

	window_bounds(cfg, bucket + 1U, &start, &end);
	*sec_until = (interval - phase) + start;
}
