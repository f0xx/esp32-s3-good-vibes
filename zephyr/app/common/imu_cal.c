#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "imu_sample.h"
#include "stall_watchdog.h"

#include "imu_cal.h"

LOG_MODULE_REGISTER(imu_cal, LOG_LEVEL_INF);

#ifndef IMU_CAL_GYRO_MAX_RMS_DPS
#define IMU_CAL_GYRO_MAX_RMS_DPS 4.0f
#endif
#ifndef IMU_CAL_GYRO_MAX_BIAS_DPS
#define IMU_CAL_GYRO_MAX_BIAS_DPS 40.0f
#endif
#ifndef IMU_CAL_ACCEL_MAX_HORIZ_G
#define IMU_CAL_ACCEL_MAX_HORIZ_G 0.18f
#endif
#ifndef IMU_CAL_ACCEL_MIN_Z_G
#define IMU_CAL_ACCEL_MIN_Z_G 0.85f
#endif

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
	float qxx = 0.0f;
	float qyy = 0.0f;
	float qzz = 0.0f;
	int collected = 0;
	int attempts = 0;

	if (cal == NULL || read_fn == NULL || samples <= 0) {
		return false;
	}

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
		qxx += sample.gx * sample.gx;
		qyy += sample.gy * sample.gy;
		qzz += sample.gz * sample.gz;
		collected++;
		if ((collected & 7) == 0) {
			stall_watchdog_feed_main();
		}
		k_msleep(10);
	}

	if (collected < samples) {
		LOG_WRN("gyro cal short n=%d/%d", collected, samples);
		return false;
	}

	const float n = (float)collected;
	const float mx = ox / n;
	const float my = oy / n;
	const float mz = oz / n;
	float vx = qxx / n - mx * mx;
	float vy = qyy / n - my * my;
	float vz = qzz / n - mz * mz;

	if (vx < 0.0f) {
		vx = 0.0f;
	}
	if (vy < 0.0f) {
		vy = 0.0f;
	}
	if (vz < 0.0f) {
		vz = 0.0f;
	}

	const float rx = sqrtf(vx);
	const float ry = sqrtf(vy);
	const float rz = sqrtf(vz);

	if (rx > IMU_CAL_GYRO_MAX_RMS_DPS || ry > IMU_CAL_GYRO_MAX_RMS_DPS ||
	    rz > IMU_CAL_GYRO_MAX_RMS_DPS) {
		LOG_WRN("gyro cal reject rms=%.2f %.2f %.2f mean=%.2f %.2f %.2f (moving)",
			(double)rx, (double)ry, (double)rz, (double)mx, (double)my, (double)mz);
		return false;
	}
	if (fabsf(mx) > IMU_CAL_GYRO_MAX_BIAS_DPS || fabsf(my) > IMU_CAL_GYRO_MAX_BIAS_DPS ||
	    fabsf(mz) > IMU_CAL_GYRO_MAX_BIAS_DPS) {
		LOG_WRN("gyro cal reject insane mean=%.2f %.2f %.2f", (double)mx, (double)my,
			(double)mz);
		return false;
	}

	LOG_INF("gyro cal mean=%.3f %.3f %.3f rms=%.2f %.2f %.2f n=%d", (double)mx, (double)my,
		(double)mz, (double)rx, (double)ry, (double)rz, collected);

	cal->gyro_off_x = mx;
	cal->gyro_off_y = my;
	cal->gyro_off_z = mz;
	cal->gyro_done = true;
	return true;
}

bool imu_cal_accel_level(struct imu_calibration *cal, bool (*read_fn)(struct imu_sample *out),
			 int samples)
{
	float sx = 0.0f;
	float sy = 0.0f;
	float sz = 0.0f;
	int collected = 0;
	int attempts = 0;

	if (cal == NULL || read_fn == NULL || samples <= 0) {
		return false;
	}

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
		if ((collected & 7) == 0) {
			stall_watchdog_feed_main();
		}
		k_msleep(10);
	}

	if (collected < samples) {
		return false;
	}

	const float n = (float)collected;
	const float mx = sx / n;
	const float my = sy / n;
	const float mz = sz / n;
	const float horiz = sqrtf(mx * mx + my * my);

	/* Only bake a "flat" zero when the die really is +Z up. Otherwise we
	 * destroy gravity and the later floor-calib matrix rotates a fake (0,0,1). */
	if (horiz > IMU_CAL_ACCEL_MAX_HORIZ_G || mz < IMU_CAL_ACCEL_MIN_Z_G) {
		LOG_WRN("accel cal skip (not +Z-up) mean=%.3f %.3f %.3f horiz=%.3f", (double)mx,
			(double)my, (double)mz, (double)horiz);
		return false;
	}

	cal->accel_off_x = mx;
	cal->accel_off_y = my;
	cal->accel_off_z = mz - 1.0f;
	cal->accel_done = true;
	return true;
}
