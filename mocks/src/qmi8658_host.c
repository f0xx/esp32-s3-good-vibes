/*
 * Host QMI8658 — synthetic IMU with first-boot bus-scan glitch (matches board:
 * first WHO_AM_I often fails after CPU PLL; firmware retries both 0x6B/0x6A).
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "host_debug.h"
#include "host_latency.h"
#include "app_func_trace.h"
#include "qmi8658.h"
#include "renderer.h"

LOG_MODULE_REGISTER(qmi8658, LOG_LEVEL_INF);

#define WHO_AM_I_VALUE 0x05
#define ADDR_HIGH      0x6B
#define ADDR_LOW       0x6A

static bool imu_ready;
static uint8_t imu_addr; /* 0 until probe succeeds — matches desk BIST addr=0x00 */
static unsigned seed = 1;
static unsigned read_count;
static unsigned init_count;
static unsigned probe_fail_injected;

static float frand(void)
{
	seed = seed * 1103515245u + 12345u;
	return ((seed >> 16) & 0x7fff) / 32768.0f - 0.5f;
}

/*
 * First qmi8658_init() after process start: fail the first two address sweeps
 * (who stays 0x00) then succeed — mirrors "first-time bus scan failure" on desk.
 * Later inits (recover/reinit) succeed on attempt 0 after settle.
 */
static bool probe_with_retries(void)
{
	static const uint8_t addrs[] = { ADDR_HIGH, ADDR_LOW };
	const int fail_rounds = (init_count == 0U) ? 2 : 0;

	host_latency_ms(HOST_LAT_QMI_SETTLE_MS);

	for (int attempt = 0; attempt < 6; attempt++) {
		for (size_t i = 0; i < sizeof(addrs); i++) {
			host_latency_us(HOST_LAT_QMI_I2C_PROBE_US);
			if (attempt < fail_rounds) {
				probe_fail_injected++;
				LOG_DBG("QMI8658 probe fail (sim bus) addr=0x%02X attempt=%d who=0x00",
					addrs[i], attempt);
				continue;
			}
			imu_addr = addrs[i];
			if (attempt > 0 || fail_rounds > 0) {
				LOG_INF("QMI8658 probe ok at 0x%02X after %d fail-rounds "
					"(%u injected WHO misses)",
					imu_addr, fail_rounds, probe_fail_injected);
			}
			return true;
		}
		host_latency_ms((uint32_t)(20 + attempt * 15));
	}

	LOG_ERR("QMI8658 not found (WHO_AM_I != 0x05) host sim");
	return false;
}

int qmi8658_init(void)
{
	APP_ENTER();
	LOG_INF("QMI8658 host probe begin");
	imu_ready = false;
	imu_addr = 0;
	read_count = 0;

	/* Desk holds ext-bus across probe + CTRL writes so SPI cannot interleave. */
	renderer_ext_bus_lock();
	if (!probe_with_retries()) {
		renderer_ext_bus_unlock();
		init_count++;
		APP_LEAVE();
		return -ENOENT;
	}
	renderer_ext_bus_unlock();

	imu_ready = true;
	host_latency_ms(HOST_LAT_QMI_DISCARD_MS);
	for (int i = 0; i < 8; i++) {
		struct qmi8658_sample discard;

		(void)qmi8658_read(&discard);
	}

	init_count++;
	LOG_INF("QMI8658 host mock ready at 0x%02X (init#%u)", imu_addr, init_count);
	APP_LEAVE();
	return 0;
}

int qmi8658_reinit(void)
{
	LOG_WRN("QMI8658 reinit");
	imu_ready = false;
	imu_addr = 0;
	host_latency_ms(20);
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
	/* Desk BIST before bring-up: who=0x00 ready=0 — same contract. */
	if (!renderer_ext_bus_try()) {
		return 0;
	}
	host_latency_us(HOST_LAT_QMI_I2C_PROBE_US);
	renderer_ext_bus_unlock();
	return imu_ready ? WHO_AM_I_VALUE : 0;
}

bool qmi8658_read(struct qmi8658_sample *out)
{
	if (!imu_ready || out == NULL) {
		LOG_DBG("qmi8658_read fail ready=%d out=%p", imu_ready ? 1 : 0, (void *)out);
		return false;
	}
	/* Skip the sample when panel SPI owns the bus (desk try-lock). */
	if (!renderer_ext_bus_try()) {
		return false;
	}

	out->ax = 0.02f + frand() * 0.01f;
	out->ay = -0.01f + frand() * 0.01f;
	out->az = 0.98f + frand() * 0.01f;
	out->gx = -0.3f + frand() * 0.4f;
	out->gy = 0.2f + frand() * 0.4f;
	out->gz = frand() * 0.3f;
	out->temp_c = 25.0f + frand();
	read_count++;
	if ((read_count % 100U) == 1U) {
		LOG_DBG("qmi8658_read #%u ax=%.3f ay=%.3f az=%.3f gx=%.2f gy=%.2f gz=%.2f",
			read_count, (double)out->ax, (double)out->ay, (double)out->az,
			(double)out->gx, (double)out->gy, (double)out->gz);
	}
	host_latency_us(HOST_LAT_QMI_READ_US);
	renderer_ext_bus_unlock();
	return true;
}
