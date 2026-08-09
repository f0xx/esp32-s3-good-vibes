#include <math.h>
#include <stdbool.h>

#include "attitude.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEG2RAD ((float)M_PI / 180.0f)

static float gyro_deadband(float dps)
{
	return fabsf(dps) < 1.0f ? 0.0f : dps;
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
		est->state.yaw += gz * DEG2RAD * dt_sec;
	}

	const struct mat3 rx = mat3_rot_x(est->state.roll);
	const struct mat3 ry = mat3_rot_y(est->state.pitch);
	const struct mat3 rz = mat3_rot_z(est->state.yaw);

	est->state.rotation = mat3_mul(rz, mat3_mul(ry, rx));
}
