#include "vibro_led.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "battery_monitor.h"
#include "device_config.h"
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
static uint8_t g_last_grb[3];
static uint8_t g_last_reason;

enum led_reason {
	LED_REASON_OFF = 0,
	LED_REASON_OK_PULSE,
	LED_REASON_BATT_FLASH,
	LED_REASON_NOK,
	LED_REASON_SETUP_BLUE,
	LED_REASON_AWAIT_FLASH,
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
		return "operational (armed, refs OK)";
	case LED_REASON_OK_PULSE:
		return "ok pulse (NVS/config saved)";
	case LED_REASON_BATT_FLASH:
		return "battery <=10% on battery power";
	case LED_REASON_NOK:
		return "verdict NOK or armed without loaded reference";
	case LED_REASON_SETUP_BLUE:
		return "setup — no reference profiles in flash";
	case LED_REASON_AWAIT_FLASH:
		return "await — refs recorded, not armed yet";
	default:
		return "?";
	}
}

/** WS2812 on this board is GRB wire order — set channels explicitly, not RGB param names. */
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
		LOG_INF("acrylic LED grb=%u,%u,%u reason=%u (%s; slots=%u armed=%d ref_len=%u)",
			green, red, blue, (unsigned)reason, reason_detail(reason),
			ref_slot_count(), device_config_vibro_armed() ? 1 : 0,
			(unsigned)vibro_capture_reference_len());
	}
	ws2812_gpio38_grb(green, red, blue);
}

static void render(void)
{
	const uint32_t now = k_uptime_get_32();
	const uint32_t ok_until = (uint32_t)atomic_get(&g_ok_until_ms);

	if (ok_until != 0U && (int32_t)(now - ok_until) < 0) {
		/* Brief blue flash — green reads as red through the red acrylic diffuser. */
		apply_grb(0U, 0U, LED_CH_BRIGHT, LED_REASON_OK_PULSE);
		return;
	}
	if (ok_until != 0U) {
		atomic_set(&g_ok_until_ms, 0);
	}

	if (battery_critical()) {
		if (flash_on_phase()) {
			apply_grb(LED_CH_BRIGHT, LED_CH_BRIGHT, 0U, LED_REASON_BATT_FLASH);
		} else {
			apply_grb(0U, 0U, 0U, LED_REASON_OFF);
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
			apply_grb(0U, 0U, 0U, LED_REASON_OFF);
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
	switch (level) {
	case VIBRO_LEVEL_ALERT:
	case VIBRO_LEVEL_WARN:
		atomic_set(&g_nok, 1);
		break;
	case VIBRO_LEVEL_OK:
	default:
		atomic_set(&g_nok, 0);
		break;
	}
}

void vibro_led_pulse_ok(void)
{
	atomic_set(&g_ok_until_ms, (atomic_val_t)(k_uptime_get_32() + LED_OK_PULSE_MS));
}

void devcfg_led_nvs_ok(void)
{
	vibro_led_pulse_ok();
}
