#include <zephyr/kernel.h>

#include "imu_sample.h"

#include "imu_cal.h"

void imu_cal_apply(struct imu_calibration *cal, struct imu_sample *sample)
{
	sample->gx -= cal->gyro_off_x;
	sample->gy -= cal->gyro_off_y;
	sample->gz -= cal->gyro_off_z;
	sample->ax -= cal->accel_off_x;
	sample->ay -= cal->accel_off_y;
	sample->az -= cal->accel_off_z;
}

bool imu_cal_gyro(struct imu_calibration *cal, bool (*read_fn)(struct imu_sample *out),
		  int samples)
{
	float ox = 0.0f;
	float oy = 0.0f;
	float oz = 0.0f;
	int collected = 0;
	int attempts = 0;

	while (collected < samples && attempts < samples * 10) {
		struct imu_sample sample;

		attempts++;
		if (!read_fn(&sample)) {
			k_msleep(5);
			continue;
		}
		ox += sample.gx;
		oy += sample.gy;
		oz += sample.gz;
		collected++;
		k_msleep(10);
	}

	if (collected == 0) {
		return false;
	}

	cal->gyro_off_x = ox / collected;
	cal->gyro_off_y = oy / collected;
	cal->gyro_off_z = oz / collected;
	cal->gyro_done = collected >= samples;
	return cal->gyro_done;
}

bool imu_cal_accel_level(struct imu_calibration *cal, bool (*read_fn)(struct imu_sample *out),
			 int samples)
{
	float sx = 0.0f;
	float sy = 0.0f;
	float sz = 0.0f;
	int collected = 0;
	int attempts = 0;

	while (collected < samples && attempts < samples * 10) {
		struct imu_sample sample;

		attempts++;
		if (!read_fn(&sample)) {
			k_msleep(5);
			continue;
		}
		sx += sample.ax;
		sy += sample.ay;
		sz += sample.az;
		collected++;
		k_msleep(10);
	}

	if (collected == 0) {
		return false;
	}

	cal->accel_off_x = sx / collected;
	cal->accel_off_y = sy / collected;
	cal->accel_off_z = (sz / collected) - 1.0f;
	cal->accel_done = collected >= samples;
	return cal->accel_done;
}
