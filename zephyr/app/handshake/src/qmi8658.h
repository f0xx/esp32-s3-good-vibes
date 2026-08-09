#pragma once

#include <stdbool.h>
#include <stdint.h>

struct qmi8658_sample {
	float ax;
	float ay;
	float az;
	float gx;
	float gy;
	float gz;
	float temp_c;
};

int qmi8658_init(void);
int qmi8658_reinit(void);
bool qmi8658_ready(void);
uint8_t qmi8658_who_am_i(void);
uint8_t qmi8658_i2c_addr(void);
bool qmi8658_read(struct qmi8658_sample *out);
