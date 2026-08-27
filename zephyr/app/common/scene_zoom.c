/*
 * Motion-reactive scene zoom — parity with esp32 scene_renderer tickZoom / updateMotionZoom.
 */

#include <math.h>

#include "scene_zoom.h"

static float g_target[3] = { SCENE_ZOOM_DEFAULT, SCENE_ZOOM_DEFAULT, SCENE_ZOOM_DEFAULT };
static float g_current[3] = { SCENE_ZOOM_DEFAULT, SCENE_ZOOM_DEFAULT, SCENE_ZOOM_DEFAULT };
static float g_movement_factor;

static float clamp_zoom(float v)
{
	if (!isfinite(v)) {
		return SCENE_ZOOM_DEFAULT;
	}
	if (v < SCENE_ZOOM_MIN) {
		return SCENE_ZOOM_MIN;
	}
	if (v > SCENE_ZOOM_MAX) {
		return SCENE_ZOOM_MAX;
	}
	return v;
}

static float clamp01(float v)
{
	if (v < 0.0f) {
		return 0.0f;
	}
	if (v > 1.0f) {
		return 1.0f;
	}
	return v;
}

static float smoothstep01(float t)
{
	t = clamp01(t);
	return t * t * (3.0f - 2.0f * t);
}

static float gyro_deadband(float dps)
{
	const float a = fabsf(dps);

	return a < 2.0f ? 0.0f : a - 2.0f;
}

static float instant_motion_activity(const struct imu_sample *sample)
{
	if (sample == NULL) {
		return 0.0f;
	}

	const float gx = gyro_deadband(sample->gx);
	const float gy = gyro_deadband(sample->gy);
	const float gz = gyro_deadband(sample->gz);
	const float gyro_dps = sqrtf(gx * gx + gy * gy + gz * gz);

	return clamp01(gyro_dps / SCENE_MOTION_GYRO_FULL_DPS);
}

static float zoom_from_motion(float motion_factor)
{
	return SCENE_ZOOM_DEFAULT -
	       motion_factor * (SCENE_ZOOM_DEFAULT - SCENE_ZOOM_MOTION_MIN);
}

static float next_zoom_nonlinear(float current, float target)
{
	if (!isfinite(current)) {
		return target;
	}
	if (current == target) {
		return current;
	}

	current += SCENE_ZOOM_SMOOTH * (target - current);
	if (fabsf(current - target) < 0.002f) {
		return target;
	}
	return clamp_zoom(current);
}

void scene_zoom_init(void)
{
	g_target[0] = SCENE_ZOOM_DEFAULT;
	g_target[1] = SCENE_ZOOM_DEFAULT;
	g_target[2] = SCENE_ZOOM_DEFAULT;
	g_current[0] = SCENE_ZOOM_DEFAULT;
	g_current[1] = SCENE_ZOOM_DEFAULT;
	g_current[2] = SCENE_ZOOM_DEFAULT;
	g_movement_factor = 0.0f;
}

void scene_zoom_tick(const struct imu_sample *sample)
{
	const float instant = instant_motion_activity(sample);
	const float k_rise = 0.40f;
	const float k_fall = 0.10f;

	if (!isfinite(g_movement_factor)) {
		g_movement_factor = 0.0f;
	}
	if (instant > g_movement_factor) {
		g_movement_factor += k_rise * (instant - g_movement_factor);
	} else {
		g_movement_factor += k_fall * (instant - g_movement_factor);
	}
	if (g_movement_factor < 0.001f) {
		g_movement_factor = 0.0f;
	}

	const float target = clamp_zoom(zoom_from_motion(smoothstep01(g_movement_factor)));

	g_target[0] = target;
	g_target[1] = target;
	g_target[2] = target;

	for (int i = 0; i < 3; i++) {
		g_current[i] = next_zoom_nonlinear(g_current[i], g_target[i]);
	}
}

const float *scene_zoom_current(void)
{
	return g_current;
}
