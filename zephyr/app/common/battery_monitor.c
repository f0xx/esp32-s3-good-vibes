/*
 * Battery monitor — parity with esp32_s3_imu_basics/battery_monitor.cpp
 */

#include "battery_monitor.h"

#include <math.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "battery_adc_esp32.h"
#include "board_config.h"
#include "device_config.h"

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

static struct battery_state g_state;
static struct device_config_v1 g_cfg;
static uint32_t g_last_sample_ms;
static uint32_t g_last_log_ms;
static float g_voltage_ema;
static float g_percent_ema;
static bool g_ema_init;
static bool g_percent_init;
static bool g_on_dc;
static float g_trend_ema;
static bool g_trend_ema_init;
static float g_v_history[32];
static uint8_t g_v_cap = BAT_TREND_WINDOW;
static uint8_t g_v_count;
static uint8_t g_v_head;
static uint8_t g_dc_streak;
static uint8_t g_bat_streak;
static uint32_t g_settle_until_ms;

static float divider_ratio(void)
{
	return (BAT_ADC_R_HIGH_OHM + BAT_ADC_R_LOW_OHM) / BAT_ADC_R_LOW_OHM;
}

static float voltage_to_percent(float v)
{
	const float pct = (v - g_cfg.bat_empty_v) / (g_cfg.bat_full_v - g_cfg.bat_empty_v) * 100.0f;

	if (pct < 0.0f) {
		return 0.0f;
	}
	if (pct > 100.0f) {
		return 100.0f;
	}
	return pct;
}

static float voltage_at_age(int age)
{
	if (g_v_count == 0 || age < 0 || age >= g_v_count) {
		return g_voltage_ema;
	}

	const int idx = ((int)g_v_head - 1 - age + (int)g_v_cap) % (int)g_v_cap;

	return g_v_history[idx];
}

static void push_voltage(float v)
{
	g_v_history[g_v_head] = v;
	g_v_head = (g_v_head + 1U) % g_v_cap;
	if (g_v_count < g_v_cap) {
		g_v_count++;
	}
}

static float trend_delta_v(void)
{
	const uint8_t compare = g_cfg.bat_trend_compare;

	if (g_v_count < compare + 1U) {
		return voltage_at_age(0) - voltage_at_age((int)g_v_count - 1);
	}

	float recent = 0.0f;
	float prior = 0.0f;

	for (uint8_t i = 0; i < compare; i++) {
		recent += voltage_at_age((int)i);
		prior += voltage_at_age((int)compare + (int)i);
	}

	return (recent - prior) / (float)compare;
}

static uint16_t read_adc_pin_mv(void)
{
	uint32_t mv_sum = 0;

	for (int i = 0; i < BAT_ADC_SAMPLES; i++) {
		uint16_t mv = 0;

		if (battery_adc_read_mv(&mv) != 0) {
			return 0;
		}
		mv_sum += mv;
	}

	return (uint16_t)(mv_sum / (uint32_t)BAT_ADC_SAMPLES);
}

static float sample_voltage_v(uint16_t *adc_mv_out)
{
	const uint16_t adc_mv = read_adc_pin_mv();

	if (adc_mv_out != NULL) {
		*adc_mv_out = adc_mv;
	}

	const float v_adc = (float)adc_mv / 1000.0f;

	return (v_adc * divider_ratio()) / g_cfg.bat_offset;
}

static void classify_power_source(void)
{
	if (!g_state.valid) {
		g_state.on_dc = false;
		return;
	}

	if (g_voltage_ema < BAT_EXTERNAL_V) {
		g_on_dc = true;
		g_state.on_dc = true;
		return;
	}

	if (g_v_count < g_cfg.bat_trend_compare + 1U) {
		if (g_voltage_ema >= g_cfg.bat_dc_margin_v) {
			g_on_dc = true;
		}
		g_state.on_dc = g_on_dc;
		return;
	}

	const float raw_trend = trend_delta_v();

	if (!g_trend_ema_init) {
		g_trend_ema = raw_trend;
		g_trend_ema_init = true;
	} else {
		g_trend_ema += 0.15f * (raw_trend - g_trend_ema);
	}

	g_state.trend_v = g_trend_ema;
	const float abs_trend = fabsf(g_trend_ema);
	bool want_dc = g_on_dc;

	if (g_trend_ema >= g_cfg.bat_trend_rise_v) {
		want_dc = true;
	} else if (g_trend_ema <= g_cfg.bat_trend_fall_v) {
		want_dc = false;
		g_on_dc = false;
		g_dc_streak = 0;
		g_bat_streak = g_cfg.bat_bat_confirm;
	} else if (abs_trend <= g_cfg.bat_trend_stable_v) {
		if (g_voltage_ema >= g_cfg.bat_dc_margin_v) {
			want_dc = true;
		} else {
			want_dc = g_on_dc;
		}
	}

	if (g_trend_ema > g_cfg.bat_trend_fall_v) {
		if (want_dc) {
			if (g_dc_streak < 255U) {
				g_dc_streak++;
			}
			g_bat_streak = 0;
			if (g_dc_streak >= g_cfg.bat_dc_confirm) {
				g_on_dc = true;
			}
		} else {
			if (g_bat_streak < 255U) {
				g_bat_streak++;
			}
			g_dc_streak = 0;
			if (g_bat_streak >= g_cfg.bat_bat_confirm) {
				g_on_dc = false;
			}
		}
	}

	g_state.on_dc = g_on_dc;
}

static void reset_trend_history(void)
{
	g_v_count = 0;
	g_v_head = 0;
	g_trend_ema = 0.0f;
	g_trend_ema_init = false;
	g_dc_streak = 0;
	g_bat_streak = 0;
}

int battery_monitor_init(void)
{
	device_config_load(&g_cfg);

	g_v_cap = g_cfg.bat_trend_window;
	if (g_v_cap < 6U) {
		g_v_cap = 6U;
	}
	if (g_v_cap > 32U) {
		g_v_cap = 32U;
	}

	memset(&g_state, 0, sizeof(g_state));
	g_on_dc = false;
	reset_trend_history();

	if (battery_adc_init() != 0) {
		LOG_WRN("battery ADC init failed");
		return -ENODEV;
	}

	{
		int raw = 0;
		uint16_t mv = 0;

		(void)battery_adc_read_raw(&raw);
		(void)battery_adc_read_mv(&mv);
		LOG_INF("battery probe raw=%d adc=%umV atten=%d cal_ok=%d", raw, mv,
			battery_adc_debug_atten(), battery_adc_cal_ok());
	}

	for (int i = 0; i < 8; i++) {
		uint16_t adc_mv = 0;
		const float v = sample_voltage_v(&adc_mv);

		push_voltage(v);
		k_msleep(5);
	}

	battery_monitor_tick();

	{
		int raw = 0;

		(void)battery_adc_read_raw(&raw);
		LOG_INF("battery init raw=%d adc=%umV v=%.2fV pct=%u%% src=%s cal_ok=%d",
			raw, g_state.adc_mv, (double)g_state.voltage_v, g_state.percent,
			g_state.on_dc ? "DC" : "BAT", battery_adc_cal_ok());
	}
	return 0;
}

void battery_monitor_settle(uint32_t ms)
{
	g_settle_until_ms = k_uptime_get_32() + ms;
}

void battery_monitor_tick(void)
{
	const uint32_t now = k_uptime_get_32();

	if (now < g_settle_until_ms) {
		return;
	}

	if ((now - g_last_sample_ms) < BAT_SAMPLE_MS) {
		return;
	}
	g_last_sample_ms = now;

	uint16_t adc_mv = 0;
	const float raw_v = sample_voltage_v(&adc_mv);

	if (!g_ema_init) {
		g_voltage_ema = raw_v;
		g_ema_init = true;
	} else {
		g_voltage_ema += g_cfg.bat_voltage_ema * (raw_v - g_voltage_ema);
	}

	push_voltage(raw_v);

	const float pct_raw = voltage_to_percent(g_voltage_ema);

	if (!g_percent_init) {
		g_percent_ema = pct_raw;
		g_percent_init = true;
	} else {
		g_percent_ema += g_cfg.bat_percent_ema * (pct_raw - g_percent_ema);
	}

	g_state.voltage_v = g_voltage_ema;
	g_state.adc_mv = adc_mv;
	g_state.valid = (adc_mv >= 30U) && (g_voltage_ema >= BAT_ADC_MIN_V);
	g_state.percent = (uint8_t)(g_percent_ema + 0.5f);
	classify_power_source();

	if (now - g_last_log_ms >= 30000U) {
		g_last_log_ms = now;
		/* dc=/bat= here are the DC/BAT *confirmation streak counters* (consecutive samples
		 * agreeing with that classification, saturating at 255) — NOT percent or millivolts.
		 * Grouped under streak(...) to avoid being misread as a second battery percentage. */
		LOG_INF("battery adc=%umV v=%.2fV pct=%u%% src=%s valid=%u trend=%+.3fV "
			"streak(dc=%u bat=%u)",
			adc_mv, (double)g_state.voltage_v, g_state.percent,
			g_state.on_dc ? "DC" : "BAT", g_state.valid, (double)g_state.trend_v,
			g_dc_streak, g_bat_streak);
	}
}

const struct battery_state *battery_monitor_state(void)
{
	return &g_state;
}
