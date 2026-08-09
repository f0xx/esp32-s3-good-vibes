/*
 * Battery ADC — Zephyr channel setup + IDF esp_adc_cal (Arduino analogReadMilliVolts parity).
 */

#include "battery_adc_esp32.h"

#include <esp_adc_cal.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bat_adc, LOG_LEVEL_INF);

#if !DT_NODE_EXISTS(DT_PATH(zephyr_user)) || !DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
#error "Board DTS must define /zephyr,user io-channels for battery sense"
#endif

static const struct adc_dt_spec g_bat_adc = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);
static esp_adc_cal_characteristics_t g_chars;
static bool g_ready;
static bool g_cal_ok;

int battery_adc_init(void)
{
	int raw = 0;
	uint16_t mv = 0;
	esp_adc_cal_value_t cal_type;

	if (!adc_is_ready_dt(&g_bat_adc)) {
		LOG_ERR("battery ADC device not ready");
		return -ENODEV;
	}

	if (adc_channel_setup_dt(&g_bat_adc) != 0) {
		LOG_ERR("battery ADC channel setup failed");
		return -EIO;
	}

	cal_type = esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100,
					    &g_chars);
	g_cal_ok = cal_type < ESP_ADC_CAL_VAL_NOT_SUPPORTED;
	g_ready = true;

	battery_adc_read_raw(&raw);
	battery_adc_read_mv(&mv);
	LOG_INF("battery probe raw=%d adc=%umV cal=%d ok=%d", raw, mv, (int)cal_type, g_cal_ok);
	return 0;
}

bool battery_adc_cal_ok(void)
{
	return g_cal_ok;
}

int battery_adc_read_raw(int *raw_out)
{
	int raw;

	if (!g_ready) {
		return -EIO;
	}

	raw = adc1_get_raw(ADC1_CHANNEL_0);
	if (raw < 0) {
		return -EIO;
	}

	if (raw_out != NULL) {
		*raw_out = raw;
	}
	return 0;
}

int battery_adc_read_mv(uint16_t *mv_out)
{
	int raw = 0;
	uint32_t mv = 0;

	if (!g_ready || !g_cal_ok) {
		return -EIO;
	}

	if (battery_adc_read_raw(&raw) != 0) {
		return -EIO;
	}

	mv = esp_adc_cal_raw_to_voltage((uint32_t)raw, &g_chars);
	if (mv_out != NULL) {
		*mv_out = (uint16_t)mv;
	}
	return 0;
}

int battery_adc_debug_atten(void)
{
	return (int)ADC_ATTEN_DB_11;
}
