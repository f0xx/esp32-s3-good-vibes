#include <math.h>
#include <stdbool.h>
#include <stddef.h>

#include "attitude.h"
#include "stack_ra_check.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEG2RAD ((float)M_PI / 180.0f)

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

static float clampf(float v, float lo, float hi)
{
	if (v < lo) {
		return lo;
	}
	if (v > hi) {
		return hi;
	}
	return v;
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

static float gyro_deadband(float dps)
{
	return fabsf(dps) < 0.35f ? 0.0f : dps;
}

/** Build quaternion from ZYX Euler (yaw*pitch*roll). */
static void quat_from_euler_zyx(struct attitude_estimator *est, float roll, float pitch, float yaw)
{
	const float cr = cosf(roll * 0.5f);
	const float sr = sinf(roll * 0.5f);
	const float cp = cosf(pitch * 0.5f);
	const float sp = sinf(pitch * 0.5f);
	const float cy = cosf(yaw * 0.5f);
	const float sy = sinf(yaw * 0.5f);

	est->q0 = cr * cp * cy + sr * sp * sy;
	est->q1 = sr * cp * cy - cr * sp * sy;
	est->q2 = cr * sp * cy + sr * cp * sy;
	est->q3 = cr * cp * sy - sr * sp * cy;
	est->state.roll = roll;
	est->state.pitch = pitch;
	est->state.yaw = yaw;
}

/** Seed quaternion from accel tilt (yaw = 0). Same atan2 forms as the old complementary filter. */
static void seed_from_accel(struct attitude_estimator *est, float ax, float ay, float az)
{
	const float roll = atan2f(ay, az);
	const float pitch = atan2f(-ax, sqrtf(ay * ay + az * az));

	quat_from_euler_zyx(est, roll, pitch, 0.0f);
	est->seeded = true;
}

static void quat_to_euler_zyx(float q0, float q1, float q2, float q3, float *roll, float *pitch,
			      float *yaw)
{
	const float sinp = 2.0f * (q0 * q2 - q3 * q1);

	*roll = atan2f(2.0f * (q0 * q1 + q2 * q3), 1.0f - 2.0f * (q1 * q1 + q2 * q2));
	*pitch = asinf(clampf(sinp, -1.0f, 1.0f));
	*yaw = atan2f(2.0f * (q0 * q3 + q1 * q2), 1.0f - 2.0f * (q2 * q2 + q3 * q3));
}

void attitude_reset(struct attitude_estimator *est)
{
	est->state.roll = 0.0f;
	est->state.pitch = 0.0f;
	est->state.yaw = 0.0f;
	est->state.rotation = mat3_identity();
	est->q0 = 1.0f;
	est->q1 = 0.0f;
	est->q2 = 0.0f;
	est->q3 = 0.0f;
	est->last_ms = 0;
	est->seeded = false;
	/* leave beta as configured by caller */
}

/**
 * Madgwick IMU update (Sebastian Madgwick, open-source AHRS).
 * Gyro in rad/s; accelerometer in g (any scale — normalized). No magnetometer.
 */
static void madgwick_imu(struct attitude_estimator *est, float gx, float gy, float gz, float ax,
			 float ay, float az, float dt)
{
	float q0 = est->q0;
	float q1 = est->q1;
	float q2 = est->q2;
	float q3 = est->q3;
	float recip_norm;
	float s0, s1, s2, s3;
	float q_dot1, q_dot2, q_dot3, q_dot4;
	float _2q0, _2q1, _2q2, _2q3, _4q0, _4q1, _4q2, _8q1, _8q2;
	float q0q0, q1q1, q2q2, q3q3;
	const float beta = est->beta > 0.0f ? est->beta : 0.1f;

	q_dot1 = 0.5f * (-q1 * gx - q2 * gy - q3 * gz);
	q_dot2 = 0.5f * (q0 * gx + q2 * gz - q3 * gy);
	q_dot3 = 0.5f * (q0 * gy - q1 * gz + q3 * gx);
	q_dot4 = 0.5f * (q0 * gz + q1 * gy - q2 * gx);

	if (!((ax == 0.0f) && (ay == 0.0f) && (az == 0.0f))) {
		recip_norm = 1.0f / sqrtf(ax * ax + ay * ay + az * az);
		ax *= recip_norm;
		ay *= recip_norm;
		az *= recip_norm;

		_2q0 = 2.0f * q0;
		_2q1 = 2.0f * q1;
		_2q2 = 2.0f * q2;
		_2q3 = 2.0f * q3;
		_4q0 = 4.0f * q0;
		_4q1 = 4.0f * q1;
		_4q2 = 4.0f * q2;
		_8q1 = 8.0f * q1;
		_8q2 = 8.0f * q2;
		q0q0 = q0 * q0;
		q1q1 = q1 * q1;
		q2q2 = q2 * q2;
		q3q3 = q3 * q3;

		/* Gradient descent corrective step (gravity reference [0,0,1]). */
		s0 = _4q0 * q2q2 + _2q2 * ax + _4q0 * q1q1 - _2q1 * ay;
		s1 = _4q1 * q3q3 - _2q3 * ax + 4.0f * q0q0 * q1 - _2q0 * ay - _4q1 +
		     _8q1 * q1q1 + _8q1 * q2q2 + _4q1 * az;
		s2 = 4.0f * q0q0 * q2 + _2q0 * ax + _4q2 * q3q3 - _2q3 * ay - _4q2 +
		     _8q2 * q1q1 + _8q2 * q2q2 + _4q2 * az;
		s3 = 4.0f * q1q1 * q3 - _2q1 * ax + 4.0f * q2q2 * q3 - _2q2 * ay;
		recip_norm = 1.0f / sqrtf(s0 * s0 + s1 * s1 + s2 * s2 + s3 * s3);
		s0 *= recip_norm;
		s1 *= recip_norm;
		s2 *= recip_norm;
		s3 *= recip_norm;

		q_dot1 -= beta * s0;
		q_dot2 -= beta * s1;
		q_dot3 -= beta * s2;
		q_dot4 -= beta * s3;
	}

	q0 += q_dot1 * dt;
	q1 += q_dot2 * dt;
	q2 += q_dot3 * dt;
	q3 += q_dot4 * dt;
	recip_norm = 1.0f / sqrtf(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
	est->q0 = q0 * recip_norm;
	est->q1 = q1 * recip_norm;
	est->q2 = q2 * recip_norm;
	est->q3 = q3 * recip_norm;
}

void attitude_update(struct attitude_estimator *est, const struct imu_sample *sample, float dt_sec)
{
	STACK_RA_CHECK_SETUP;
	float roll, pitch, yaw;

	if (dt_sec <= 0.0f || sample == NULL) {
		return;
	}

	const float ax = sample->ax;
	const float ay = sample->ay;
	const float az = sample->az;
	const float accel_mag = sqrtf(ax * ax + ay * ay + az * az);

	if (!est->seeded) {
		if (accel_mag < 0.2f || !isfinite(accel_mag)) {
			return;
		}
		seed_from_accel(est, ax, ay, az);
		attitude_rotation_zyx(&est->state.rotation, est->state.roll, est->state.pitch,
				     est->state.yaw);
		return;
	}

	const float gx = gyro_deadband(sample->gx);
	const float gy = gyro_deadband(sample->gy);
	const float gz = gyro_deadband(sample->gz);
	const float gyro_mag = sqrtf(gx * gx + gy * gy + gz * gz);
	const bool stationary = gyro_mag < 1.5f && fabsf(accel_mag - 1.0f) < 0.12f;

	/*
	 * No magnetometer: residual gyro bias spins the cube when sitting still.
	 * When quiet, snap tilt from accel and freeze yaw (same idea as the old
	 * complementary filter) instead of integrating Madgwick on biased gyros.
	 */
	if (stationary) {
		const float roll_acc = atan2f(ay, az);
		const float pitch_acc = atan2f(-ax, sqrtf(ay * ay + az * az));

		quat_from_euler_zyx(est, roll_acc, pitch_acc, est->state.yaw);
		attitude_rotation_zyx(&est->state.rotation, est->state.roll, est->state.pitch,
				     est->state.yaw);
	} else {
		/* Madgwick expects rad/s. Sample gx/gy/gz are deg/s (pipeline convention). */
		madgwick_imu(est, gx * DEG2RAD, gy * DEG2RAD, gz * DEG2RAD, ax, ay, az, dt_sec);
		quat_to_euler_zyx(est->q0, est->q1, est->q2, est->q3, &roll, &pitch, &yaw);
		est->state.roll = roll;
		est->state.pitch = pitch;
		est->state.yaw = wrap_pi(yaw);
		attitude_rotation_zyx(&est->state.rotation, est->state.roll, est->state.pitch,
				     est->state.yaw);
	}

	if (!isfinite(est->state.roll) || !isfinite(est->state.pitch) ||
	    !isfinite(est->state.yaw) || !isfinite(est->q0) ||
	    !isfinite(est->state.rotation.m[0][0])) {
		attitude_reset(est);
	}
	STACK_RA_CHECK();
}
