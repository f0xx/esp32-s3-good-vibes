#pragma once

#include <stdbool.h>

#include "imu_math.h"
#include "imu_sample.h"

struct attitude_state {
	float roll;
	float pitch;
	float yaw;
	struct mat3 rotation;
};

/**
 * Madgwick AHRS (IMU / 6DOF — no magnetometer).
 * beta: gradient-descent gain (typical 0.05–0.15). Higher = trust accel more, less gyro drift
 * on tilt, more noise. Yaw remains gyro-only and will drift.
 */
struct attitude_estimator {
	struct attitude_state state;
	float q0;
	float q1;
	float q2;
	float q3;
	float beta;
	int64_t last_ms;
	bool seeded;
};

void attitude_reset(struct attitude_estimator *est);
void attitude_update(struct attitude_estimator *est, const struct imu_sample *sample,
		     float dt_sec);
/** Closed-form ZYX (yaw*pitch*roll) matrix — same convention as attitude_update(). */
void attitude_rotation_zyx(struct mat3 *out, float roll, float pitch, float yaw);
