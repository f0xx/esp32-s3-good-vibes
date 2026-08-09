#include "chip_temp.h"

#include <math.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>

#include "board_config.h"

#if DT_HAS_COMPAT_STATUS_OKAY(espressif_esp32_temp)
#define HAS_CHIP_TEMP 1
static const struct device *const chip_temp_dev = DEVICE_DT_GET(DT_NODELABEL(coretemp));
#else
#define HAS_CHIP_TEMP 0
#endif

static float g_celsius;
static bool g_valid;
static uint32_t g_last_ms;

int chip_temp_init(void)
{
#if HAS_CHIP_TEMP
	if (!device_is_ready(chip_temp_dev)) {
		return -ENODEV;
	}
#endif
	g_valid = false;
	g_last_ms = 0;
	return 0;
}

void chip_temp_tick(void)
{
#if HAS_CHIP_TEMP
	const uint32_t now = k_uptime_get_32();

	if (g_last_ms != 0 && now - g_last_ms < CHIP_TEMP_SAMPLE_MS) {
		return;
	}
	g_last_ms = now;

	if (sensor_sample_fetch(chip_temp_dev) != 0) {
		g_valid = false;
		return;
	}

	struct sensor_value val;

	if (sensor_channel_get(chip_temp_dev, SENSOR_CHAN_DIE_TEMP, &val) != 0) {
		g_valid = false;
		return;
	}

	const float t = (float)val.val1 + (float)val.val2 / 1000000.0f;

	if (isfinite(t) && t > -40.0f && t < 125.0f) {
		g_celsius = t;
		g_valid = true;
	} else {
		g_valid = false;
	}
#endif
}

float chip_temp_celsius(void)
{
	return g_celsius;
}

bool chip_temp_valid(void)
{
	return g_valid;
}
