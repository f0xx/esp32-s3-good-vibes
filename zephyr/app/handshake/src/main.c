/*
 * Waveshare ESP32-S3-LCD-1.47B — mobile app handshake + live scene (Zephyr parity)
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>

#include "battery_monitor.h"
#include "board_config.h"
#include "chip_temp.h"
#include "ble_config_gatt.h"
#include "ble_crash_gatt.h"
#include "ble_looper.h"
#include "device_config.h"
#include "battery_bench.h"
#include "ble_imu_gatt.h"
#include "ble_net_gatt.h"
#include "ble_ota_gatt.h"
#include "boot_button.h"
#include "bist.h"
#include "crash_debug.h"
#include "crash_report.h"
#include "fw_version.h"
#include "display_panel.h"
#include "imu_pipeline.h"
#include "network_manager.h"
#include "ota_ab.h"
#include "panel_backlight.h"
#include "panel_fb.h"
#include "power_manager.h"
#include "scene_live.h"
#include "scene_zoom.h"
#include "soft_reboot.h"
#include "radio_scheduler.h"
#include "stall_watchdog.h"
#include "clock_sync.h"
#include "vibro_capture.h"
#include "vibro_schedule.h"
#include "ws2812_gpio38.h"
#include "vibro_led.h"
#include "mt200_bridge.h"

LOG_MODULE_REGISTER(handshake, LOG_LEVEL_INF);

static const struct device *const display_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

#define RENDER_THREAD_STACK 8192
#define RENDER_THREAD_PRIO  5

static K_THREAD_STACK_DEFINE(render_stack, RENDER_THREAD_STACK);
static struct k_thread render_thread;

static uint32_t last_hb_ms;
static uint32_t hb_render_frames;
static uint32_t render_next_ms;

/*
 * Render-thread stage marker — diagnostic only, added after repeated intermittent "render
 * stall panic" (EXCCAUSE 63, always the same generic k_panic() PC — that's the *main* thread's
 * panic call site, not where the render thread actually got stuck, so it tells us nothing on
 * its own). Recorded by render_thread_fn before/after each of its blocking calls so the next
 * occurrence's "render stalled" log line names the actual hung call and how long it's been
 * stuck there, instead of just "frames==0 for 20s".
 */
enum render_stage {
	RENDER_STAGE_IDLE = 0,
	RENDER_STAGE_SYNC,
	RENDER_STAGE_BACKLIGHT,
	RENDER_STAGE_SNAPSHOT,
	RENDER_STAGE_ZOOM,
	RENDER_STAGE_DRAW,
	RENDER_STAGE_SLEEP,
};

static volatile uint8_t g_render_stage;
static volatile uint32_t g_render_stage_ms;

static const char *render_stage_name(uint8_t stage)
{
	switch (stage) {
	case RENDER_STAGE_SYNC:
		return "power_manager_sync_render";
	case RENDER_STAGE_BACKLIGHT:
		return "panel_backlight_reapply";
	case RENDER_STAGE_SNAPSHOT:
		return "imu_pipeline_snapshot";
	case RENDER_STAGE_ZOOM:
		return "scene_zoom_tick";
	case RENDER_STAGE_DRAW:
		return "scene_live_draw";
	case RENDER_STAGE_SLEEP:
		return "k_msleep";
	default:
		return "idle";
	}
}

static void render_stage_mark(enum render_stage stage)
{
	g_render_stage = (uint8_t)stage;
	g_render_stage_ms = k_uptime_get_32();
}

static bool panel_hw_apply(bool on)
{
	return panel_display_hw_set(display_dev, on);
}

static void ws2812_off(void)
{
	for (int i = 0; i < 3; i++) {
		ws2812_gpio38_off();
		k_msleep(2);
	}
}

static void start_ble(void)
{
	int err = bt_enable(NULL);

	if (err) {
		LOG_ERR("Bluetooth init failed (%d)", err);
		return;
	}

	power_manager_set_bt_controller_on(true);
	stall_watchdog_feed_main();
	ble_net_gatt_init();
	ble_config_gatt_init();
	ble_crash_gatt_init();
	ble_ota_gatt_init();
	ble_imu_gatt_init();
	stall_watchdog_feed_main();
	(void)ble_looper_post_adv_start();
	stall_watchdog_feed_main();
	power_manager_reapply_backlight_if_screen_on();
	stall_watchdog_feed_main();
	LOG_INF("BLE advertising scheduled");
}

static void render_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	stall_watchdog_arm_render();
	render_next_ms = k_uptime_get_32();

	while (1) {
		const uint32_t now = k_uptime_get_32();
		const uint32_t period = power_manager_render_interval_ms();

		stall_watchdog_feed_render();
		render_stage_mark(RENDER_STAGE_SYNC);
		power_manager_sync_render();

		if (power_manager_tft_render_enabled()) {
			struct imu_sample sample;

			render_stage_mark(RENDER_STAGE_BACKLIGHT);
			panel_backlight_reapply();
			render_stage_mark(RENDER_STAGE_SNAPSHOT);
			if (imu_pipeline_snapshot(&sample, NULL)) {
				render_stage_mark(RENDER_STAGE_ZOOM);
				scene_zoom_tick(&sample);
			}
			render_stage_mark(RENDER_STAGE_DRAW);
			scene_live_draw(display_dev);
			hb_render_frames++;
			stall_watchdog_feed_render();
		}

		render_next_ms += period;
		if ((int32_t)(render_next_ms - now) <= 0) {
			render_next_ms = now + period;
		}

		const int32_t sleep_ms = (int32_t)(render_next_ms - k_uptime_get_32());

		render_stage_mark(RENDER_STAGE_SLEEP);
		k_msleep(sleep_ms > 0 ? (uint32_t)sleep_ms : 1U);
	}
}

int main(void)
{
	printk("handshake: main()\n");

	(void)panel_backlight_init();
	boot_button_init();
	ws2812_off();

	if (!device_is_ready(display_dev)) {
		LOG_ERR("display not ready");
		return 0;
	}

	LOG_INF("stage: settings init");
	stall_watchdog_feed_main();
	(void)settings_subsys_init();
	device_config_init();
	soft_reboot_init();
	ota_ab_init();

	crash_report_init();
	ota_ab_on_boot();
	soft_reboot_post_boot();
	stall_watchdog_feed_main();

	LOG_INF("stage: config load");
	{
		struct device_config_v1 cfg;

		(void)device_config_load(&cfg);
	}

	LOG_INF("stage: battery/chip temp");
	(void)battery_monitor_init();
	(void)chip_temp_init();

	LOG_INF("stage: power manager");
	power_manager_set_display(display_dev);
	power_manager_set_panel_hw_fn(panel_hw_apply);
	power_manager_set_display_busy_query(panel_display_busy);
	power_manager_set_imu_reschedule_cb(imu_pipeline_reschedule);
	power_manager_init();
	battery_bench_init();
	radio_scheduler_init(ble_imu_gatt_set_traffic_paused);

	LOG_INF("stage: network manager");
	network_manager_init();

	LOG_INF("%s — MCUboot A/B OTA", FW_VERSION_NAME);

	LOG_INF("stage: clock / NTP scheduler");
	clock_sync_ntp_init();

	LOG_INF("stage: stall watchdog");
	if (stall_watchdog_init() != 0) {
		LOG_WRN("stall watchdog init failed");
	}

	LOG_INF("stage: IMU pipeline");
	if (!imu_pipeline_init()) {
		LOG_WRN("IMU pipeline init failed");
	}
	stall_watchdog_feed_main();
#if defined(CONFIG_APP_CRASH_DEBUG)
	bist_run();
#endif
	scene_live_init(display_dev);
	vibro_led_init();
	stall_watchdog_feed_main();

	crash_report_persist_boot_crash();

	LOG_INF("stage: BLE");
	(void)ble_looper_init();
	start_ble();
	mt200_bridge_autostart();
	stall_watchdog_feed_main();

	power_manager_mark_ready();
	stall_watchdog_feed_main();

	k_thread_create(&render_thread, render_stack, K_THREAD_STACK_SIZEOF(render_stack),
			render_thread_fn, NULL, NULL, NULL, RENDER_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&render_thread, "render");

	LOG_INF("stage: main loop (BOOT tap = backlight toggle)");

	while (1) {
		const uint32_t now = k_uptime_get_32();

		if (boot_button_take_toggle_request()) {
			if (!battery_bench_config_locked()) {
				stall_watchdog_feed_main();
				power_manager_toggle_mode();
				stall_watchdog_feed_main();
			}
		}
		boot_button_poll();
		crash_debug_poll();
		ble_config_gatt_poll();
		ble_looper_poll();
		imu_pipeline_poll();
		device_config_poll();
		ota_ab_poll();
		clock_sync_ntp_poll();
		battery_monitor_tick();
		if (battery_bench_safety_poll()) {
			ble_imu_gatt_bench_sample();
		}
		chip_temp_tick();
		power_manager_tick();
		if (battery_bench_tick(now)) {
			ble_imu_gatt_bench_sample();
		}
		network_manager_tick();
		ble_net_gatt_tick();
		vibro_capture_session_tick(now);
		vibro_capture_poll();
		vibro_led_poll();
		{
			const struct device_config_v1 *dcfg = device_config_runtime();

			radio_scheduler_set_capture_prep(
				vibro_schedule_capture_prep_active(dcfg, clock_sync_now_ms32()));
		}

		stall_watchdog_feed_main();
		if (!power_manager_tft_render_enabled()) {
			stall_watchdog_feed_render();
		}

		if (last_hb_ms == 0U) {
			last_hb_ms = now;
		} else if (now - last_hb_ms >= 10000U) {
			const uint32_t window = now - last_hb_ms;
			const bool screen_on = power_manager_tft_render_enabled();

			const uint32_t imu_ticks = imu_pipeline_take_hb_ticks();

			stall_watchdog_hb_window(hb_render_frames, screen_on, imu_ticks);

			if (screen_on && hb_render_frames == 0U) {
				const uint8_t stage = g_render_stage;
				char spi_info[64];

				(void)panel_fb_flush_stall_info(spi_info, sizeof(spi_info));
				LOG_WRN("render stalled (%ums) — nudge panel on render thread "
					"(stuck@%s for %ums) spi: %s",
					window, render_stage_name(stage),
					now - g_render_stage_ms, spi_info);
				panel_backlight_reapply();
				power_manager_request_panel_hw(true);
			}

			if (screen_on && imu_ticks == 0U && power_manager_imu_hz_target() > 0U) {
				LOG_WRN("IMU stalled (%ums) — recover", window);
				imu_pipeline_request_recover();
			}

			power_manager_log_telemetry(hb_render_frames, imu_ticks, window,
						    panel_fb_last_flush_ms());
			imu_pipeline_log_motion();
			hb_render_frames = 0;
			last_hb_ms = now;
		}

		k_msleep(power_manager_main_sleep_ms());
	}

	return 0;
}
