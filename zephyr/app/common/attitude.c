#include <math.h>
#include <stdbool.h>

#include "attitude.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEG2RAD ((float)M_PI / 180.0f)

static float gyro_deadband(float dps)
{
	return fabsf(dps) < 0.35f ? 0.0f : dps;
}

static float wrap_pi(float rad)
{
	while (rad > (float)M_PI) {
		rad -= 2.0f * (float)M_PI;
	}
	while (rad < -(float)M_PI) {
		rad += 2.0f * (float)M_PI;
	}
	return rad;
}

void attitude_rotation_zyx(struct mat3 *out, float roll, float pitch, float yaw)
{
	const float cr = cosf(roll);
	const float sr = sinf(roll);
	const float cp = cosf(pitch);
	const float sp = sinf(pitch);
	const float cy = cosf(yaw);
	const float sy = sinf(yaw);

	if (out == NULL) {
		return;
	}

	*out = (struct mat3){
		.m = {
			{ cy * cp, cy * sp * sr - sy * cr, cy * sp * cr + sy * sr },
			{ sy * cp, sy * sp * sr + cy * cr, sy * sp * cr - cy * sr },
			{ -sp, cp * sr, cp * cr },
		},
	};
}

void attitude_reset(struct attitude_estimator *est)
{
	est->state.roll = 0.0f;
	est->state.pitch = 0.0f;
	est->state.yaw = 0.0f;
	est->state.rotation = mat3_identity();
	est->last_ms = 0;
}

void attitude_update(struct attitude_estimator *est, const struct imu_sample *sample, float dt_sec)
{
	if (dt_sec <= 0.0f || sample == NULL) {
		return;
	}

	const float ax = sample->ax;
	const float ay = sample->ay;
	const float az = sample->az;
	const float roll_acc = atan2f(ay, az);
	const float pitch_acc = atan2f(-ax, sqrtf(ay * ay + az * az));

	const float gx = gyro_deadband(sample->gx);
	const float gy = gyro_deadband(sample->gy);
	const float gz = gyro_deadband(sample->gz);
	const float gyro_mag = sqrtf(gx * gx + gy * gy + gz * gz);
	const float accel_mag = sqrtf(ax * ax + ay * ay + az * az);
	const bool stationary = gyro_mag < 1.5f && fabsf(accel_mag - 1.0f) < 0.12f;

	if (stationary) {
		est->state.roll = roll_acc;
		est->state.pitch = pitch_acc;
	} else {
		est->state.roll = (1.0f - est->alpha) * (est->state.roll + gx * DEG2RAD * dt_sec) +
				  est->alpha * roll_acc;
		est->state.pitch = (1.0f - est->alpha) * (est->state.pitch + gy * DEG2RAD * dt_sec) +
				   est->alpha * pitch_acc;
	}
	/* No magnetometer — yaw is gyro-only. The old "stationary" branch skipped this, so a
	 * slow desk turn (deadbanded gyro, accel still ~1 g) froze the cube while the HUD/tick
	 * kept moving. Always integrate, even when still. */
	est->state.yaw = wrap_pi(est->state.yaw + gz * DEG2RAD * dt_sec);

	attitude_rotation_zyx(&est->state.rotation, est->state.roll, est->state.pitch,
			      est->state.yaw);

	if (!isfinite(est->state.roll) || !isfinite(est->state.pitch) ||
	    !isfinite(est->state.yaw) || !isfinite(est->state.rotation.m[0][0])) {
		attitude_reset(est);
	}
}
