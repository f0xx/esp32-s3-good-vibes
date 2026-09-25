/*
 * Battery ADC — Zephyr ADC DT channel only (no IDF esp_adc_cal; removed in
 * Zephyr 4.x / newer Espressif HAL ports).
 */

#include "battery_adc_esp32.h"

#include <stddef.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(bat_adc, LOG_LEVEL_INF);

#if !DT_NODE_EXISTS(DT_PATH(zephyr_user)) || !DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
#error "Board DTS must define /zephyr,user io-channels for battery sense"
#endif

static const struct adc_dt_spec g_bat_adc = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);
static bool g_ready;

int battery_adc_init(void)
{
	int raw = 0;
	uint16_t mv = 0;

	if (!adc_is_ready_dt(&g_bat_adc)) {
		LOG_ERR("battery ADC device not ready");
		return -ENODEV;
	}

	if (adc_channel_setup_dt(&g_bat_adc) != 0) {
		LOG_ERR("battery ADC channel setup failed");
		return -EIO;
	}

	g_ready = true;

	(void)battery_adc_read_raw(&raw);
	(void)battery_adc_read_mv(&mv);
	LOG_INF("battery probe raw=%d adc=%umV (zephyr adc_raw_to_millivolts_dt)", raw, mv);
	return 0;
}

bool battery_adc_cal_ok(void)
{
	return g_ready;
}

int battery_adc_read_raw(int *raw_out)
{
	int16_t buf = 0;
	struct adc_sequence seq = {
		.buffer = &buf,
		.buffer_size = sizeof(buf),
	};
	int err;

	if (!g_ready) {
		return -EIO;
	}

	err = adc_sequence_init_dt(&g_bat_adc, &seq);
	if (err) {
		return err;
	}
	err = adc_read_dt(&g_bat_adc, &seq);
	if (err) {
		return err;
	}

	if (raw_out != NULL) {
		*raw_out = (int)buf;
	}
	return 0;
}

int battery_adc_read_mv(uint16_t *mv_out)
{
	int raw = 0;
	int32_t mv = 0;
	int err;

	if (!g_ready) {
		return -EIO;
	}

	err = battery_adc_read_raw(&raw);
	if (err) {
		return err;
	}

	mv = raw;
	err = adc_raw_to_millivolts_dt(&g_bat_adc, &mv);
	if (err) {
		return err;
	}

	if (mv_out != NULL) {
		*mv_out = (uint16_t)CLAMP(mv, 0, 65535);
	}
	return 0;
}

int battery_adc_debug_atten(void)
{
	return 11; /* historical ADC_ATTEN_DB_11 marker for logs */
}
