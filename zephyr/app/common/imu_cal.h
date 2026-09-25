#pragma once

#include <stdbool.h>

#include "imu_sample.h"

struct imu_calibration {
	float gyro_off_x;
	float gyro_off_y;
	float gyro_off_z;
	float accel_off_x;
	float accel_off_y;
	float accel_off_z;
	bool gyro_done;
	bool accel_done;
};

void imu_cal_apply(struct imu_calibration *cal, struct imu_sample *sample);
/** Average still gyro. Rejects (and does not write offsets) if the board is
 *  moving, reads fail, or the implied bias is implausible. */
bool imu_cal_gyro(struct imu_calibration *cal, bool (*read_fn)(struct imu_sample *out),
		  int samples);
/** Zero accel as +Z-up / 1 g. Rejects unless the averaged gravity is actually
 *  close to (0, 0, +1) — do not call this when a floor-calib matrix is active. */
bool imu_cal_accel_level(struct imu_calibration *cal, bool (*read_fn)(struct imu_sample *out),
			 int samples);
