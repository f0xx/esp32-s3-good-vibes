#include "walk_distance.h"

#include <math.h>
#include <zephyr/sys/util.h>

static const float k_mag_slow_alpha = 0.012f;
static const float k_vert_base_alpha = 0.015f;
static const float k_hp_smooth_alpha = 0.28f;
static const float k_vert_slow_alpha = 0.008f;

enum {
	ELEVATOR_HOLD_MAX = 120,
	MIN_STEP_INTERVAL_MS = 420,
	MAX_STEP_INTERVAL_MS = 2200,
	CADENCE_WINDOW_MS = 3000,
	MIN_ARM_DURATION_MS = 80,
	MAX_ARM_DURATION_MS = 420,
	IDLE_GAP_MS = 2500,
};

static const float k_elevator_vert_g = 0.14f;
static const float k_gyro_veto_dps = 70.0f;
static const float k_shake_peak_ceiling = 0.20f;

static float pocket_threshold_scale(const struct walk_distance_estimator *est)
{
	const float hip_h = est->cfg.user_height_m * 0.53f;

	if (hip_h < 0.5f) {
		return 1.0f;
	}

	const float ratio = est->cfg.pocket_height_m / hip_h;

	if (ratio < 0.85f) {
		return 1.10f;
	}
	if (ratio > 1.35f) {
		return 0.90f;
	}
	return 1.0f;
}

static float surface_peak_high(const struct walk_distance_estimator *est)
{
	float thr;

	switch (est->state.surface) {
	case WALK_SURFACE_FLAT_SOFT:
		thr = 0.048f;
		break;
	case WALK_SURFACE_UNEVEN:
		thr = 0.072f;
		break;
	case WALK_SURFACE_FLAT_HARD:
	default:
		thr = 0.055f;
		break;
	}
	return thr * pocket_threshold_scale(est);
}

static float surface_peak_low(const struct walk_distance_estimator *est)
{
	float thr;

	switch (est->state.surface) {
	case WALK_SURFACE_FLAT_SOFT:
		thr = 0.018f;
		break;
	case WALK_SURFACE_UNEVEN:
		thr = 0.030f;
		break;
	case WALK_SURFACE_FLAT_HARD:
	default:
		thr = 0.022f;
		break;
	}
	return thr * pocket_threshold_scale(est);
}

static float surface_step_scale(const struct walk_distance_estimator *est)
{
	switch (est->state.surface) {
	case WALK_SURFACE_FLAT_SOFT:
		return 0.95f;
	case WALK_SURFACE_UNEVEN:
		return 0.92f;
	case WALK_SURFACE_FLAT_HARD:
	default:
		return 1.00f;
	}
}

static float estimate_step_length(const struct walk_distance_estimator *est, float peak_g)
{
	const float height_stride = est->cfg.user_height_m * 0.415f;
	const float thr = surface_peak_high(est);
	const float intensity = (peak_g - thr) / (thr + 0.15f);
	const float t = intensity < 0.0f ? 0.0f : (intensity > 1.0f ? 1.0f : intensity);
	float step = est->cfg.step_min_m + t * (est->cfg.step_max_m - est->cfg.step_min_m);

	step = 0.55f * step + 0.45f * height_stride;

	if (est->state.cadence_hz > 1.0f && est->state.cadence_hz < 3.5f) {
		const float cadence_delta = est->state.cadence_hz - 2.0f;

		step += cadence_delta * 0.025f;
	}

	step *= surface_step_scale(est);

	if (step < est->cfg.step_min_m) {
		step = est->cfg.step_min_m;
	}
	if (step > est->cfg.step_max_m) {
		step = est->cfg.step_max_m;
	}
	return step;
}

static void update_terrain_gate(struct walk_distance_estimator *est, const struct imu_sample *sample,
				const struct attitude_state *att, float dt_sec)
{
	const struct vec3 body = vec3_make(sample->ax, sample->ay, sample->az);
	const struct vec3 world = mat3_transform(&att->rotation, body);
	const float lin_vert = world.z - 1.0f;

	est->vert_slow_ema =
		(1.0f - k_vert_slow_alpha) * est->vert_slow_ema + k_vert_slow_alpha * lin_vert;

	if (fabsf(est->vert_slow_ema) > k_elevator_vert_g) {
		if (est->elevator_hold < ELEVATOR_HOLD_MAX) {
			est->elevator_hold += 1;
		}
	} else if (est->elevator_hold > 0) {
		est->elevator_hold -= 1;
	}

	est->state.counting = est->elevator_hold < (ELEVATOR_HOLD_MAX / 3);
	ARG_UNUSED(dt_sec);
}

static void register_step(struct walk_distance_estimator *est, float peak_g, uint32_t now_ms)
{
	float step_m = estimate_step_length(est, peak_g);

	if (est->last_step_ms == 0 || now_ms - est->last_step_ms > IDLE_GAP_MS) {
		step_m *= 0.45f;
	}

	est->state.steps += 1;
	est->state.last_step_m = step_m;
	est->state.distance_m += step_m;
	est->state.peak_g = peak_g;
	est->state.walking = true;
	est->last_step_ms = now_ms;

	if (est->last_cadence_ms == 0 || now_ms - est->last_cadence_ms > CADENCE_WINDOW_MS) {
		est->last_cadence_ms = now_ms;
		est->cadence_window_steps = 1;
		est->state.cadence_hz = 0.0f;
	} else {
		est->cadence_window_steps += 1;
		const float elapsed_s = (now_ms - est->last_cadence_ms) / 1000.0f;

		if (elapsed_s > 0.05f) {
			est->state.cadence_hz = (float)est->cadence_window_steps / elapsed_s;
		}
	}
}

void walk_distance_init(struct walk_distance_estimator *est,
			const struct walk_distance_config *cfg)
{
	est->cfg = *cfg;
	walk_distance_reset(est);
}

void walk_distance_reset(struct walk_distance_estimator *est)
{
	est->state.steps = 0;
	est->state.distance_m = 0.0f;
	est->state.last_step_m = 0.0f;
	est->state.cadence_hz = 0.0f;
	est->state.step_signal = 0.0f;
	est->state.peak_g = 0.0f;
	est->state.walking = false;
	est->state.counting = true;
	est->state.surface = WALK_SURFACE_FLAT_HARD;
	est->mag_slow_ema = 1.0f;
	est->mag_hp_lp = 0.0f;
	est->vert_base_ema = 0.0f;
	est->vert_hp_lp = 0.0f;
	est->vert_slow_ema = 0.0f;
	est->elevator_hold = 0;
	est->armed = false;
	est->peak_armed = 0.0f;
	est->armed_since_ms = 0;
	est->last_step_ms = 0;
	est->last_cadence_ms = 0;
	est->cadence_window_steps = 0;
}

void walk_distance_set_config(struct walk_distance_estimator *est,
			      const struct walk_distance_config *cfg)
{
	est->cfg = *cfg;
}

void walk_distance_set_surface(struct walk_distance_estimator *est, enum walk_surface surface)
{
	est->state.surface = surface;
}

void walk_distance_update(struct walk_distance_estimator *est, const struct imu_sample *sample,
			  const struct attitude_state *att, float dt_sec, uint32_t now_ms)
{
	if (dt_sec <= 0.0f || sample == NULL || att == NULL) {
		return;
	}

	update_terrain_gate(est, sample, att, dt_sec);

	const float mag = sqrtf(sample->ax * sample->ax + sample->ay * sample->ay +
				sample->az * sample->az);

	est->mag_slow_ema = (1.0f - k_mag_slow_alpha) * est->mag_slow_ema + k_mag_slow_alpha * mag;
	const float mag_hp = mag - est->mag_slow_ema;

	est->mag_hp_lp = (1.0f - k_hp_smooth_alpha) * est->mag_hp_lp + k_hp_smooth_alpha * mag_hp;

	const struct vec3 body = vec3_make(sample->ax, sample->ay, sample->az);
	const struct vec3 world = mat3_transform(&att->rotation, body);
	const float lin_vert = world.z - 1.0f;

	est->vert_base_ema =
		(1.0f - k_vert_base_alpha) * est->vert_base_ema + k_vert_base_alpha * lin_vert;
	const float vert_hp = lin_vert - est->vert_base_ema;

	est->vert_hp_lp = (1.0f - k_hp_smooth_alpha) * est->vert_hp_lp + k_hp_smooth_alpha * vert_hp;

	const float mag_bounce = est->mag_hp_lp >= 0.0f ? est->mag_hp_lp : -est->mag_hp_lp;
	const float vert_bounce = est->vert_hp_lp >= 0.0f ? est->vert_hp_lp : -est->vert_hp_lp;
	const float gyro_dps =
		sqrtf(sample->gx * sample->gx + sample->gy * sample->gy + sample->gz * sample->gz);
	const float step_signal = 0.55f * mag_bounce + 0.45f * vert_bounce;

	est->state.step_signal = step_signal;

	if (est->last_step_ms != 0 && now_ms - est->last_step_ms > MAX_STEP_INTERVAL_MS) {
		est->state.walking = false;
		est->state.cadence_hz = 0.0f;
	}

	if (!est->state.counting || gyro_dps > k_gyro_veto_dps) {
		est->armed = false;
		est->peak_armed = 0.0f;
		est->armed_since_ms = 0;
		return;
	}

	const float thr_high = surface_peak_high(est);
	const float thr_low = surface_peak_low(est);

	if (!est->armed) {
		if (step_signal >= thr_high) {
			est->armed = true;
			est->peak_armed = step_signal;
			est->armed_since_ms = now_ms;
		}
	} else {
		if (step_signal > est->peak_armed) {
			est->peak_armed = step_signal;
		}
		if (step_signal <= thr_low) {
			const uint32_t arm_ms = now_ms - est->armed_since_ms;
			const bool spaced = est->last_step_ms == 0 ||
					    (now_ms - est->last_step_ms >= MIN_STEP_INTERVAL_MS);
			const bool shaped =
				arm_ms >= MIN_ARM_DURATION_MS && arm_ms <= MAX_ARM_DURATION_MS;
			const bool not_shake = est->peak_armed <= k_shake_peak_ceiling;

			if (spaced && shaped && not_shake && est->peak_armed >= thr_high) {
				register_step(est, est->peak_armed, now_ms);
			}
			est->armed = false;
			est->peak_armed = 0.0f;
			est->armed_since_ms = 0;
		}
	}
}

const struct walk_distance_state *walk_distance_state_get(
	const struct walk_distance_estimator *est)
{
	return &est->state;
}
