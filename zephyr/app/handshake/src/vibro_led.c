#include "vibro_led.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "battery_monitor.h"
#include "device_config.h"
#include "panel_backlight.h"
#include "vibro_capture.h"
#include "vibro_ref_store.h"
#include "ws2812_gpio38.h"

LOG_MODULE_REGISTER(vibro_led, LOG_LEVEL_INF);

#define LED_FLASH_PERIOD_MS   4000U
#define LED_FLASH_ON_MS       2000U
#define LED_OK_PULSE_MS       2000U
#define LED_BATTERY_CRIT_PCT  10U

#define LED_CH_BRIGHT 48U

static atomic_t g_nok;
static atomic_t g_ok_until_ms;
static atomic_t g_debug_on;
static atomic_t g_debug_mask;
static uint8_t g_last_grb[3];
static uint8_t g_last_reason;

enum led_reason {
	LED_REASON_OFF = 0,
	LED_REASON_OK_PULSE,
	LED_REASON_BATT_FLASH,
	LED_REASON_NOK,
	LED_REASON_SETUP_BLUE,
	LED_REASON_AWAIT_FLASH,
	LED_REASON_DEBUG,
};

static uint8_t ref_slot_count(void)
{
	uint8_t n = 0U;

	for (uint8_t s = 0U; s < VIBRO_REF_STORE_SLOTS; s++) {
		if (vibro_ref_store_valid(s)) {
			n++;
		}
	}
	return n;
}

static bool flash_on_phase(void)
{
	return (k_uptime_get_32() % LED_FLASH_PERIOD_MS) < LED_FLASH_ON_MS;
}

static bool battery_critical(void)
{
	const struct battery_state *bat = battery_monitor_state();

	if (bat == NULL || !bat->valid || bat->on_dc) {
		return false;
	}
	return bat->percent <= LED_BATTERY_CRIT_PCT;
}

static bool missing_reference_nok(void)
{
	/* No flash profiles → wizard/setup, not an operational fault. */
	if (ref_slot_count() == 0U) {
		return false;
	}
	if (!device_config_vibro_armed()) {
		return false;
	}
	if (vibro_capture_reference_recording()) {
		return false;
	}
	return !vibro_capture_reference_ready();
}

static const char *reason_detail(enum led_reason reason)
{
	switch (reason) {
	case LED_REASON_OFF:
		return "off — operational (armed, refs OK)";
	case LED_REASON_OK_PULSE:
		return "ok pulse (NVS/config saved)";
	case LED_REASON_BATT_FLASH:
		return "battery <=10% on battery power";
	case LED_REASON_NOK:
		return "armed without loaded reference";
	case LED_REASON_SETUP_BLUE:
		return "setup — no reference profiles in flash";
	case LED_REASON_AWAIT_FLASH:
		return "await — refs recorded, not armed yet";
	case LED_REASON_DEBUG:
		return "debug RGB override (phone checkboxes)";
	default:
		return "?";
	}
}

/** Logical GRB in; RGB on the wire (this pixel is not Arduino-default GRB). */
static void apply_grb(uint8_t green, uint8_t red, uint8_t blue, enum led_reason reason)
{
	if (g_last_grb[0] == green && g_last_grb[1] == red && g_last_grb[2] == blue &&
	    reason == g_last_reason) {
		return;
	}

	g_last_grb[0] = green;
	g_last_grb[1] = red;
	g_last_grb[2] = blue;
	if (reason != g_last_reason) {
		g_last_reason = reason;
		LOG_INF("acrylic LED rgb=%u,%u,%u reason=%u (%s; slots=%u armed=%d ref_len=%u)",
			red, green, blue, (unsigned)reason, reason_detail(reason),
			ref_slot_count(), device_config_vibro_armed() ? 1 : 0,
			(unsigned)vibro_capture_reference_len());
	}
	ws2812_gpio38_rgb(red, green, blue);
}

static void render(void)
{
	const uint32_t now = k_uptime_get_32();
	const uint32_t ok_until = (uint32_t)atomic_get(&g_ok_until_ms);
	const uint8_t dbg = (uint8_t)atomic_get(&g_debug_mask);
	const bool debug_on = atomic_get(&g_debug_on) != 0;

	if (debug_on) {
		const uint8_t r = (dbg & 1U) ? LED_CH_BRIGHT : 0U;
		const uint8_t g = (dbg & 2U) ? LED_CH_BRIGHT : 0U;
		const uint8_t b = (dbg & 4U) ? LED_CH_BRIGHT : 0U;

		apply_grb(g, r, b, LED_REASON_DEBUG);
		return;
	}

	if (ok_until != 0U && (int32_t)(now - ok_until) < 0) {
		apply_grb(LED_CH_BRIGHT, 0U, 0U, LED_REASON_OK_PULSE);
		return;
	}
	if (ok_until != 0U) {
		atomic_set(&g_ok_until_ms, 0);
	}

	if (battery_critical()) {
		if (flash_on_phase()) {
			apply_grb(LED_CH_BRIGHT, LED_CH_BRIGHT, 0U, LED_REASON_BATT_FLASH);
		} else {
			/* Keep BATT_FLASH as the reason so the off half of the 2s/2s
			 * blink is not logged as "operational (armed, refs OK)". */
			apply_grb(0U, 0U, 0U, LED_REASON_BATT_FLASH);
		}
		return;
	}

	if (ref_slot_count() == 0U) {
		apply_grb(0U, 0U, LED_CH_BRIGHT, LED_REASON_SETUP_BLUE);
		return;
	}

	if (!device_config_vibro_armed()) {
		if (flash_on_phase()) {
			apply_grb(0U, 0U, LED_CH_BRIGHT, LED_REASON_AWAIT_FLASH);
		} else {
			apply_grb(0U, 0U, 0U, LED_REASON_AWAIT_FLASH);
		}
		return;
	}

	if (atomic_get(&g_nok) != 0 || missing_reference_nok()) {
		apply_grb(0U, LED_CH_BRIGHT, 0U, LED_REASON_NOK);
		return;
	}

	apply_grb(0U, 0U, 0U, LED_REASON_OFF);
}

void vibro_led_init(void)
{
	atomic_set(&g_nok, 0);
	atomic_set(&g_ok_until_ms, 0);
	atomic_set(&g_debug_on, 0);
	atomic_set(&g_debug_mask, 0);
	g_last_grb[0] = g_last_grb[1] = g_last_grb[2] = 255U;
	g_last_reason = 255U;
	(void)ws2812_gpio38_init();
	if (device_config_vibro_armed() && ref_slot_count() == 0U) {
		LOG_WRN("vibro armed in NVS but no reference slots — clearing arm");
		device_config_set_vibro_armed(false);
	}
	render();
	LOG_INF("acrylic LED schema: blue=setup flash-blue=await red=nok yellow=batt green-pulse=ok off=run");
}

void vibro_led_poll(void)
{
	render();
}

void vibro_led_on_verdict(enum vibro_level level)
{
	/* Per-window candidate (vd) never pages the acrylic LED. Backend
	 * operator_alert is the mechanic page; missing-ref NOK still applies. */
	ARG_UNUSED(level);
	atomic_set(&g_nok, 0);
}

void vibro_led_pulse_ok(void)
{
	atomic_set(&g_ok_until_ms, (atomic_val_t)(k_uptime_get_32() + LED_OK_PULSE_MS));
}

void vibro_led_debug_set(bool active, uint8_t mask)
{
	mask &= 7U;
	atomic_set(&g_debug_on, active ? 1 : 0);
	atomic_set(&g_debug_mask, mask);
	g_last_grb[0] = g_last_grb[1] = g_last_grb[2] = 255U;
	g_last_reason = 255U;
	/* Hold LCD backlight only (ST7789 TFT). Keep scene rendering so leave is just BL on. */
	panel_backlight_hold_off(active);
	if (!active) {
		ws2812_gpio38_off();
		LOG_INF("LED debug off — schema and LCD backlight resumed");
	} else {
		LOG_INF("LED debug on mask=0x%x (R=%d G=%d B=%d) LCD BL held off", mask,
			(mask & 1) ? 1 : 0, (mask & 2) ? 1 : 0, (mask & 4) ? 1 : 0);
	}
	render();
}

void devcfg_led_nvs_ok(void)
{
	vibro_led_pulse_ok();
}
