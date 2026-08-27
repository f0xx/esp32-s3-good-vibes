/*
 * QMI8658 6-axis IMU — minimal Zephyr I2C port of esp32_s3_imu_basics/qmi8658_imu.cpp
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "qmi8658.h"

LOG_MODULE_REGISTER(qmi8658, LOG_LEVEL_INF);

#define REG_WHO_AM_I  0x00
#define REG_CTRL1     0x02
#define REG_CTRL2     0x03
#define REG_CTRL3     0x04
#define REG_CTRL7     0x08
#define REG_STATUS0   0x2E
#define REG_TEMP_L    0x33
#define REG_AX_L      0x35

#define WHO_AM_I_VALUE 0x05
#define ADDR_HIGH      0x6B
#define ADDR_LOW       0x6A

#define ACCEL_LSB 4096.0f
#define GYRO_LSB  64.0f

static const struct device *const i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));

static uint8_t imu_addr;
static bool imu_ready;

static int16_t combine(uint8_t lo, uint8_t hi)
{
	return (int16_t)((uint16_t)hi << 8 | lo);
}

static bool reg_write(uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = {reg, val};

	return i2c_write(i2c_dev, buf, sizeof(buf), imu_addr) == 0;
}

static bool reg_read(uint8_t reg, uint8_t *data, size_t len)
{
	return i2c_write_read(i2c_dev, imu_addr, &reg, 1, data, len) == 0;
}

static bool probe_addr(uint8_t addr)
{
	uint8_t reg = REG_WHO_AM_I;
	uint8_t id = 0;

	if (i2c_write_read(i2c_dev, addr, &reg, 1, &id, 1) != 0) {
		return false;
	}

	if (id != WHO_AM_I_VALUE) {
		return false;
	}

	imu_addr = addr;
	return true;
}

int qmi8658_init(void)
{
	if (!device_is_ready(i2c_dev)) {
		LOG_ERR("I2C0 not ready");
		return -ENODEV;
	}

	if (!probe_addr(ADDR_HIGH) && !probe_addr(ADDR_LOW)) {
		LOG_ERR("QMI8658 not found (WHO_AM_I != 0x05)");
		return -ENOENT;
	}

	if (!reg_write(REG_CTRL1, 0x60) || !reg_write(REG_CTRL2, (0x02 << 4) | 0x05) ||
	    !reg_write(REG_CTRL3, (0x04 << 4) | 0x05) || !reg_write(REG_CTRL7, 0x03)) {
		LOG_ERR("QMI8658 register init failed");
		return -EIO;
	}

	imu_ready = true;
	LOG_INF("QMI8658 ready at 0x%02X", imu_addr);
	return 0;
}

int qmi8658_reinit(void)
{
	imu_ready = false;
	k_msleep(20);
	return qmi8658_init();
}

bool qmi8658_ready(void)
{
	return imu_ready;
}

uint8_t qmi8658_i2c_addr(void)
{
	return imu_addr;
}

uint8_t qmi8658_who_am_i(void)
{
	uint8_t reg = REG_WHO_AM_I;
	uint8_t id = 0;

	if (!device_is_ready(i2c_dev) || !imu_ready) {
		return 0;
	}

	if (i2c_write_read(i2c_dev, imu_addr, &reg, 1, &id, 1) != 0) {
		return 0;
	}

	return id;
}

bool qmi8658_read(struct qmi8658_sample *out)
{
	uint8_t raw[12];

	if (!imu_ready || out == NULL) {
		return false;
	}

	for (int attempt = 0; attempt < 4; attempt++) {
		uint8_t status = 0;

		if (!reg_read(REG_STATUS0, &status, 1) || (status & 0x03) == 0) {
			k_usleep(400);
			continue;
		}

		if (reg_read(REG_AX_L, raw, sizeof(raw))) {
			goto parsed;
		}

		k_usleep(400);
	}

	return false;

parsed:

	out->ax = combine(raw[0], raw[1]) / ACCEL_LSB;
	out->ay = combine(raw[2], raw[3]) / ACCEL_LSB;
	out->az = combine(raw[4], raw[5]) / ACCEL_LSB;
	out->gx = combine(raw[6], raw[7]) / GYRO_LSB;
	out->gy = combine(raw[8], raw[9]) / GYRO_LSB;
	out->gz = combine(raw[10], raw[11]) / GYRO_LSB;

	uint8_t temp_raw[2];

	if (reg_read(REG_TEMP_L, temp_raw, sizeof(temp_raw))) {
		out->temp_c = (float)combine(temp_raw[0], temp_raw[1]) / 256.0f;
	} else {
		out->temp_c = 0.0f;
	}

	return true;
}
