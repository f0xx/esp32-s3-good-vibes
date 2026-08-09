#include "panel_backlight.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>

#include "board_config.h"

LOG_MODULE_REGISTER(panel_bl, LOG_LEVEL_INF);

#define BACKLIGHT_NODE DT_ALIAS(lcd_backlight)

#if DT_NODE_HAS_STATUS(BACKLIGHT_NODE, okay)
static const struct pwm_dt_spec bl_pwm = PWM_DT_SPEC_GET(BACKLIGHT_NODE);
#else
#error "lcd-backlight alias missing in devicetree"
#endif

static uint8_t g_percent = PANEL_TFT_BL_DEFAULT_PERCENT;
static bool g_on = true;

static int apply_pwm(uint8_t percent)
{
	if (!device_is_ready(bl_pwm.dev)) {
		return -ENODEV;
	}

	if (percent > PANEL_TFT_BL_MAX_PERCENT) {
		percent = PANEL_TFT_BL_MAX_PERCENT;
	}

	/* pwm_set_pulse_dt() expects nanoseconds, not raw duty cycles */
	const uint32_t pulse_ns =
		(uint32_t)((uint64_t)bl_pwm.period * percent / 100U);

	return pwm_set_pulse_dt(&bl_pwm, pulse_ns);
}

int panel_backlight_init(void)
{
	if (!device_is_ready(bl_pwm.dev)) {
		LOG_ERR("backlight PWM not ready");
		return -ENODEV;
	}

	g_on = true;
	g_percent = PANEL_TFT_BL_DEFAULT_PERCENT;
	const int err = apply_pwm(g_percent);

	if (err == 0) {
		LOG_INF("backlight on at %u%%", g_percent);
	} else {
		LOG_ERR("backlight PWM apply failed (%d)", err);
	}
	return err;
}

void panel_backlight_reapply(void)
{
	if (!device_is_ready(bl_pwm.dev)) {
		return;
	}

	if (g_on) {
		uint8_t percent = g_percent;

		if (percent > PANEL_TFT_BL_MAX_PERCENT) {
			percent = PANEL_TFT_BL_MAX_PERCENT;
		}

		/* BT/WiFi radio init can reset LEDC — re-set period + duty (Arduino ledcAttach parity). */
		const uint32_t pulse_ns =
			(uint32_t)((uint64_t)bl_pwm.period * percent / 100U);

		(void)pwm_set_dt(&bl_pwm, bl_pwm.period, pulse_ns);
	}
}

void panel_backlight_set_on(bool on)
{
	g_on = on;
	if (on) {
		(void)apply_pwm(g_percent);
	} else {
		(void)pwm_set_pulse_dt(&bl_pwm, 0);
	}
}

bool panel_backlight_is_on(void)
{
	return g_on;
}

void panel_backlight_set_percent(uint8_t percent)
{
	g_percent = percent;
	if (g_on) {
		(void)apply_pwm(g_percent);
	}
}

uint8_t panel_backlight_percent(void)
{
	return g_percent;
}
