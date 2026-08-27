#pragma once

#include "imu_math.h"
#include "imu_sample.h"

struct attitude_state {
	float roll;
	float pitch;
	float yaw;
	struct mat3 rotation;
};

struct attitude_estimator {
	struct attitude_state state;
	float alpha;
	int64_t last_ms;
};

void attitude_reset(struct attitude_estimator *est);
void attitude_update(struct attitude_estimator *est, const struct imu_sample *sample,
		     float dt_sec);
/** Closed-form ZYX (yaw*pitch*roll) matrix — same as attitude_update(). */
void attitude_rotation_zyx(struct mat3 *out, float roll, float pitch, float yaw);
