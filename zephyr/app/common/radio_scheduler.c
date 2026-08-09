#include "radio_scheduler.h"

#include <zephyr/logging/log.h>

#include "network_manager.h"

LOG_MODULE_REGISTER(radio_sched, LOG_LEVEL_INF);

static bool g_wifi_busy;
static bool g_capture_prep;
static radio_ble_pause_fn g_ble_pause;

static void apply_pause(void)
{
	const bool pause = g_wifi_busy || g_capture_prep;

	if (g_ble_pause != NULL) {
		g_ble_pause(pause);
	}
}

void radio_scheduler_init(radio_ble_pause_fn pause_fn)
{
	g_ble_pause = pause_fn;
	g_wifi_busy = false;
	g_capture_prep = false;
}

void radio_scheduler_set_wifi_busy(bool busy)
{
	if (g_wifi_busy == busy) {
		return;
	}

	g_wifi_busy = busy;
	apply_pause();
	LOG_INF("radio %s — IMU BLE traffic %s", busy ? "wifi" : "ble",
		busy ? "paused" : "active");
}

void radio_scheduler_set_capture_prep(bool prep)
{
	if (g_capture_prep == prep) {
		return;
	}

	g_capture_prep = prep;
	apply_pause();
	if (prep) {
		LOG_INF("radio prep — IMU BLE traffic paused (capture T-%us)",
			(unsigned)VIBRO_PRE_CAPTURE_PAUSE_SEC);
	}
}

void radio_scheduler_sync(void)
{
	radio_scheduler_set_wifi_busy(network_manager_radio_busy());
}

bool radio_scheduler_wifi_busy(void)
{
	return g_wifi_busy;
}

bool radio_scheduler_capture_prep(void)
{
	return g_capture_prep;
}

const char *radio_scheduler_mode_str(void)
{
	if (g_wifi_busy) {
		return "wifi";
	}
	if (g_capture_prep) {
		return "prep";
	}
	return "ble";
}
