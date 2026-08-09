#pragma once

#include <stdint.h>

struct imu_sample {
	float ax;
	float ay;
	float az;
	float gx;
	float gy;
	float gz;
	float temp_c;
};
