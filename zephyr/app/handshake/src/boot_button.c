#include "boot_button.h"

#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include "board_config.h"
#include "ble_imu_gatt.h"
#include "device_config.h"
#include "net_profile_store.h"
#include "power_manager.h"

LOG_MODULE_REGISTER(boot_btn, LOG_LEVEL_INF);

static bool g_prev_pressed;
static bool g_long_handled;
static bool g_armed;
static bool g_seen_release;
static uint32_t g_down_ms;
static volatile bool g_pressed;

#define BOOT_GRACE_MS 3500U

static bool g_warn_wifi_erase;
static bool g_warn_nvs_erase;

static void input_cb(struct input_event *evt)
{
	if (evt->type != INPUT_EV_KEY || evt->code != INPUT_KEY_0) {
		return;
	}

	g_pressed = (evt->value != 0);
}

INPUT_CALLBACK_DEFINE(NULL, input_cb);

void boot_button_init(void)
{
	g_prev_pressed = false;
	g_long_handled = false;
	g_armed = false;
	g_seen_release = false;
	g_down_ms = 0;
	g_pressed = false;
	LOG_INF("BOOT button via INPUT subsystem");
}

void boot_button_sync(void)
{
	g_prev_pressed = g_pressed;
	g_seen_release = !g_prev_pressed;
	g_armed = true;
	LOG_INF("BOOT armed (released=%d)", g_prev_pressed ? 0 : 1);
}

bool boot_button_screen_on(void)
{
	return power_manager_screen_on();
}

static bool g_mode_toggle_pending;

bool boot_button_take_toggle_request(void)
{
	if (!g_mode_toggle_pending) {
		return false;
	}

	g_mode_toggle_pending = false;
	return true;
}

static void toggle_mode(void)
{
	g_mode_toggle_pending = true;
	LOG_INF("BOOT tap → demo/staging mode toggle queued");
}

static void erase_nvs_and_reboot(void)
{
	if (ble_imu_link_active()) {
		LOG_WRN("BOOT 15s: NVS erase blocked — disconnect phone first");
		return;
	}

	LOG_WRN("BOOT held 15s — erasing settings NVS, cold reboot follows");
	if (device_config_storage_erase()) {
		struct device_config_v1 cfg;

		device_config_defaults(&cfg);
		(void)device_config_save_sync(&cfg);
	}
	LOG_WRN("BOOT: sys_reboot (NVS erase complete)");
	k_msleep(100);
	sys_reboot(SYS_REBOOT_COLD);
}

static void erase_wifi_and_reboot(void)
{
	if (ble_imu_link_active()) {
		LOG_WRN("BOOT 10s: WiFi erase blocked — disconnect phone first");
		return;
	}

	LOG_WRN("BOOT held 10s — erasing WiFi profiles, cold reboot follows");
	net_profile_store_clear_all();
	LOG_WRN("BOOT: sys_reboot (WiFi erase complete)");
	k_msleep(100);
	sys_reboot(SYS_REBOOT_COLD);
}

void boot_button_poll(void)
{
	const uint32_t now = k_uptime_get_32();
	const bool pressed = g_pressed;

	if (now < BOOT_GRACE_MS) {
		return;
	}

	if (!g_armed) {
		boot_button_sync();
		return;
	}

	if (!g_seen_release) {
		if (!pressed) {
			g_seen_release = true;
			g_prev_pressed = false;
		} else if (now >= BOOT_GRACE_MS + 2000U) {
			/* GPIO0 can read pressed at boot — don't block taps forever. */
			g_seen_release = true;
			g_prev_pressed = true;
			LOG_WRN("BOOT: assuming released (stuck-low at boot)");
		}
		return;
	}

	if (pressed && !g_prev_pressed) {
		g_down_ms = now;
		g_long_handled = false;
		g_warn_wifi_erase = false;
		g_warn_nvs_erase = false;
	} else if (pressed && g_prev_pressed && !g_long_handled && g_down_ms > 0U) {
		const uint32_t held = now - g_down_ms;

		if (held >= 9000U && held < NET_BOOT_ERASE_MS) {
			if (!g_warn_wifi_erase) {
				LOG_WRN("BOOT ~10s — release now or WiFi profiles will be erased");
				g_warn_wifi_erase = true;
			}
		}
		if (held >= 14000U && held < NVS_BOOT_ERASE_MS) {
			if (!g_warn_nvs_erase) {
				LOG_WRN("BOOT ~15s — release now or NVS will be erased");
				g_warn_nvs_erase = true;
			}
		}
		if (held >= NVS_BOOT_ERASE_MS) {
			g_long_handled = true;
			if (ble_imu_link_active()) {
				LOG_WRN("BOOT 15s held — NVS erase skipped (BLE linked)");
			} else {
				erase_nvs_and_reboot();
			}
			return;
		}
		if (held >= NET_BOOT_ERASE_MS) {
			g_long_handled = true;
			if (ble_imu_link_active()) {
				LOG_WRN("BOOT 10s held — WiFi erase skipped (BLE linked)");
			} else {
				erase_wifi_and_reboot();
			}
			return;
		}
	} else if (!pressed && g_prev_pressed && !g_long_handled) {
		const uint32_t held = now - g_down_ms;

		if (held >= 50 && held < NET_BOOT_ERASE_MS) {
			LOG_INF("BOOT tap (%ums) → toggle demo/staging mode", held);
			toggle_mode();
		} else if (held >= NET_BOOT_ERASE_MS) {
			LOG_WRN("BOOT release after %ums (need %u/%u ms for erase)",
				held, NET_BOOT_ERASE_MS, NVS_BOOT_ERASE_MS);
		}
	}

	g_prev_pressed = pressed;
}
