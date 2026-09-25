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

#include "app_func_trace.h"
#include "renderer.h"
#include "stack_ra_check.h"
#include "stall_watchdog.h"

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

/* STATUS0: bit0 = accel data available, bit1 = gyro data available. Accepting
 * accel-only after CTRL7 enable left gyro registers at 0 / stale — boot cal
 * then stored a ~0 offset and the scene yaw-spun from residual bias. */
#define STATUS_ACCEL_DA 0x01
#define STATUS_GYRO_DA  0x02
#define STATUS_BOTH_DA  (STATUS_ACCEL_DA | STATUS_GYRO_DA)

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
	STACK_RA_CHECK_SETUP;
	uint8_t buf[2] = {reg, val};
	const bool ok = i2c_write(i2c_dev, buf, sizeof(buf), imu_addr) == 0;

	STACK_RA_CHECK();
	return ok;
}

static bool reg_read(uint8_t reg, uint8_t *data, size_t len)
{
	STACK_RA_CHECK_SETUP;
	const bool ok = i2c_write_read(i2c_dev, imu_addr, &reg, 1, data, len) == 0;

	STACK_RA_CHECK();
	return ok;
}

/* One WHO_AM_I attempt. On I2C error id stays 0 and rc is the driver errno. */
static int probe_addr_once(uint8_t addr, uint8_t *id_out)
{
	STACK_RA_CHECK_SETUP;
	uint8_t reg = REG_WHO_AM_I;
	uint8_t id = 0;
	int rc;

	*id_out = 0;
	/* Caller must hold ext-bus — bare I2C during panel SPI DoubleException'd. */
	rc = i2c_write_read(i2c_dev, addr, &reg, 1, &id, 1);
	*id_out = id;
	STACK_RA_CHECK();
	if (rc != 0) {
		return rc;
	}
	if (id != WHO_AM_I_VALUE) {
		return -ENOENT;
	}

	imu_addr = addr;
	return 0;
}

/*
 * First probe often fails right after power_manager CPU PLL retap (bus glitch /
 * settle). Retry both addresses with short backoff before giving up.
 */
static bool probe_with_retries(void)
{
	static const uint8_t addrs[] = { ADDR_HIGH, ADDR_LOW };
	uint8_t last_id[2] = { 0, 0 };
	int last_rc[2] = { -EIO, -EIO };

	APP_ENTER();
	STACK_RA_CHECK_SETUP;
	/* Brief settle after boot / CPU frequency change. */
	k_msleep(30);

	for (int attempt = 0; attempt < 6; attempt++) {
		STACK_RA_CHECK();
		for (size_t i = 0; i < ARRAY_SIZE(addrs); i++) {
			last_rc[i] = probe_addr_once(addrs[i], &last_id[i]);
			if (last_rc[i] == 0) {
				if (attempt > 0) {
					LOG_INF("QMI8658 probe ok at 0x%02X after %d retries",
						addrs[i], attempt);
				}
				STACK_RA_CHECK();
				APP_LEAVE();
				return true;
			}
		}
		stall_watchdog_feed_main();
		/* Bus wedged after a CPU retap — kick the ESP32 I2C FSM before retry. */
		if (attempt == 1 || attempt == 3) {
			(void)i2c_recover_bus(i2c_dev);
		}
		k_msleep(20 + (attempt * 15));
	}

	LOG_ERR("QMI8658 not found (WHO_AM_I != 0x05) "
		"0x%02X who=0x%02X rc=%d 0x%02X who=0x%02X rc=%d",
		addrs[0], last_id[0], last_rc[0], addrs[1], last_id[1], last_rc[1]);
	APP_LEAVE();
	return false;
}

int qmi8658_init(void)
{
	int discards_ok = 0;

	APP_ENTER();
	if (!device_is_ready(i2c_dev)) {
		LOG_ERR("I2C0 not ready");
		APP_LEAVE();
		return -ENODEV;
	}

	LOG_INF("QMI8658 probe begin");
	/* Hold ext-bus across probe + CTRL writes — unlocked probe raced SPI (v311–v313). */
	renderer_ext_bus_lock();
	(void)i2c_recover_bus(i2c_dev);

	if (!probe_with_retries()) {
		renderer_ext_bus_unlock();
		APP_LEAVE();
		return -ENOENT;
	}

	if (!reg_write(REG_CTRL1, 0x60) || !reg_write(REG_CTRL2, (0x02 << 4) | 0x05) ||
	    !reg_write(REG_CTRL3, (0x04 << 4) | 0x05) || !reg_write(REG_CTRL7, 0x03)) {
		renderer_ext_bus_unlock();
		LOG_ERR("QMI8658 register init failed");
		APP_LEAVE();
		return -EIO;
	}
	renderer_ext_bus_unlock();

	imu_ready = true;

	/* Gyro drive-start: first STATUS0 hits are accel-only, then a few dps of
	 * transient on gz. Discard those before anyone averages a bias.
	 * Cap attempts — a wedged bus used to burn ~160s (16×20×500ms I2C timeouts)
	 * with g_recovering stuck true and a frozen cube. */
	k_msleep(80);
	for (int i = 0; i < 8; i++) {
		struct qmi8658_sample discard;

		if (qmi8658_read(&discard)) {
			discards_ok++;
		}
		if ((i & 1) == 0) {
			stall_watchdog_feed_main();
		}
	}
	if (discards_ok == 0) {
		LOG_ERR("QMI8658 post-init reads failed — bus wedged?");
		imu_ready = false;
		(void)i2c_recover_bus(i2c_dev);
		APP_LEAVE();
		return -EIO;
	}

	LOG_INF("QMI8658 ready at 0x%02X (discard_ok=%d)", imu_addr, discards_ok);
	APP_LEAVE();
	return 0;
}

int qmi8658_reinit(void)
{
	APP_ENTER();
	imu_ready = false;
	k_msleep(20);
	{
		const int rc = qmi8658_init();

		APP_LEAVE();
		return rc;
	}
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
	STACK_RA_CHECK_SETUP;
	uint8_t reg = REG_WHO_AM_I;
	uint8_t id = 0;

	if (!device_is_ready(i2c_dev) || !imu_ready) {
		return 0;
	}

	if (!renderer_ext_bus_try()) {
		return 0;
	}
	if (i2c_write_read(i2c_dev, imu_addr, &reg, 1, &id, 1) != 0) {
		id = 0;
	}
	renderer_ext_bus_unlock();
	STACK_RA_CHECK();
	return id;
}

bool qmi8658_read(struct qmi8658_sample *out)
{
	STACK_RA_CHECK_SETUP;
	uint8_t raw[12];
	uint8_t i2c_fail = 0;
	bool ok = false;

	if (!imu_ready || out == NULL) {
		return false;
	}

	/* Serialize vs panel SPI — concurrent MSPI/I2C smashed SP (v310–v312). */
	if (!renderer_ext_bus_try()) {
		return false;
	}

	/* Status-not-ready is cheap (usleep). I2C timeout is ~500ms — fail fast so
	 * recover/calibrate cannot sit in a multi-minute silence loop. */
	for (int attempt = 0; attempt < 12; attempt++) {
		uint8_t status = 0;

		STACK_RA_CHECK();
		if (!reg_read(REG_STATUS0, &status, 1)) {
			if (++i2c_fail >= 3) {
				goto out;
			}
			k_usleep(400);
			continue;
		}
		if ((status & STATUS_BOTH_DA) != STATUS_BOTH_DA) {
			k_usleep(400);
			continue;
		}

		if (reg_read(REG_AX_L, raw, sizeof(raw))) {
			goto parsed;
		}
		if (++i2c_fail >= 3) {
			goto out;
		}

		k_usleep(400);
	}

	goto out;

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

	STACK_RA_CHECK();
	ok = true;

out:
	renderer_ext_bus_unlock();
	return ok;
}
