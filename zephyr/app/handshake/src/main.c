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
#include <stdint.h>

#include "battery_monitor.h"
#include "app_func_trace.h"
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
#include "crash_alive.h"
#include "crash_debug.h"
#include "crash_report.h"
#include "fw_version.h"
#include "display_panel.h"
#include "imu_pipeline.h"
#include "network_manager.h"
#include "ota_ab.h"
#include "ota_channel.h"
#include "ota_wifi.h"
#include "panel_backlight.h"
#include "renderer.h"
#include "power_manager.h"
#include "scene_live.h"
#include "soft_reboot.h"
#include "radio_scheduler.h"
#include "stall_watchdog.h"
#include "clock_sync.h"
#include "vibro_capture.h"
#include "vibro_schedule.h"
#include "ws2812_gpio38.h"
#include "vibro_led.h"
#include "mt200_bridge.h"
#include "mt200_sample_queue.h"
#include "stack_ra_check.h"

LOG_MODULE_REGISTER(handshake, LOG_LEVEL_INF);

static const struct device *const display_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

/* 20 KiB still saw Invalid SP / illegal insn on render (desk); give more headroom
 * for scene_live_draw + SPI strip buffers + I2C frames under BLE load. */
#define RENDER_THREAD_STACK 32768
#define RENDER_THREAD_PRIO  5

static K_THREAD_STACK_DEFINE(render_stack, RENDER_THREAD_STACK);
static struct k_thread render_thread;

static uint32_t last_hb_ms;
static uint32_t hb_render_frames;
/* Lifetime frame counter for GDB hang-hunt (hb_render_frames resets every 10s). */
volatile uint32_t g_render_frames_total;
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
/* Last main-loop step name — if UART dies mid-call, this is the hung frame. */
static volatile const char *g_main_step = "boot";
static volatile uint32_t g_main_loops;
#define MAIN_STEP(name)                                                                            \
	do {                                                                                       \
		g_main_step = (name);                                                              \
	} while (0)

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
	/* Queues if called off the render thread; applies immediately on it. */
	return renderer_request_hw(on);
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
	STACK_RA_CHECK_SETUP;
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	stall_watchdog_arm_render();
	renderer_claim_thread();
	renderer_drain(); /* boot HW/present queued from main */
	render_next_ms = k_uptime_get_32();

	while (1) {
		STACK_RA_CHECK();
		const uint32_t now = k_uptime_get_32();
		uint32_t period = power_manager_render_interval_ms();

		/*
		 * Exclusive-epoch panel SPI: pause only while radio_scheduler holds
		 * the panel (MT200 session / WiFi / capture-prep). Phone link must
		 * keep drawing — gating on ble_imu_link_active froze the cube.
		 */
		const bool spi_ok =
			radio_scheduler_panel_spi_allowed() && !renderer_spi_tripped();

		if (!spi_ok) {
			/* Soft trip + radio idle → clear and fall through to redraw.
			 * v307 permanent trip froze the cube while phone AHRS moved. */
			if (renderer_spi_tripped() && radio_scheduler_panel_spi_allowed()) {
				renderer_spi_clear_trip();
			} else {
				/* Never cut SPI mid-flush — wait out in-flight present. */
				if (renderer_busy()) {
					render_stage_mark(RENDER_STAGE_SYNC);
					renderer_drain();
					k_msleep(5);
					continue;
				}
				renderer_set_spi_enabled(false);
				if (period < 500U) {
					period = 500U;
				}
				stall_watchdog_feed_render();
				render_stage_mark(RENDER_STAGE_SLEEP);
				k_msleep(period);
				continue;
			}
		}
		renderer_set_spi_enabled(true);
		/* Cap panel duty — full ~21 Hz fights MSPI under residual BLE. */
		if (period < 200U) {
			period = 200U;
		}
		/* Phone link: keep cube alive but ease MSPI pressure. */
		if (ble_imu_link_active() && period < 250U) {
			period = 250U;
		}
		/* While MT200 feature is armed, keep a gentler floor (~5 Hz). */
		if (mt200_bridge_wanted() && period < 200U) {
			period = 200U;
		}

		stall_watchdog_feed_render();

		/*
		 * flash_area_erase on main — skip SPI drain AND panel HW sync.
		 * Draining present/blanking mid-erase fights MSPI with flash and
		 * has wedged the S3 under dual-role BLE.
		 */
		if (scene_live_flash_quiet()) {
			stall_watchdog_feed_render();
			k_msleep(20);
			continue;
		}

		render_stage_mark(RENDER_STAGE_SYNC);
		renderer_drain();
		power_manager_sync_render();

		if (power_manager_tft_render_enabled()) {
			render_stage_mark(RENDER_STAGE_BACKLIGHT);
			panel_backlight_reapply();
			/* One snapshot inside scene_live_draw (zoom + cube) —
			 * avoid double imu_lock under SPI/I2C pressure. */
			render_stage_mark(RENDER_STAGE_DRAW);
			scene_live_draw(display_dev);
			hb_render_frames++;
			g_render_frames_total++;
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
	STACK_RA_CHECK_SETUP;
	printk("handshake: main()\n");

	(void)panel_backlight_init();
	boot_button_init();
	ws2812_off();

	if (!device_is_ready(display_dev)) {
		LOG_ERR("display not ready");
		return 0;
	}
	renderer_init(display_dev);
	/* Queue unblank — SPI runs on render thread after claim/drain. */
	(void)renderer_request_hw(true);

	LOG_INF("stage: settings init");
	stall_watchdog_feed_main();
	(void)settings_subsys_init();
	device_config_init();
	soft_reboot_init();
	ota_channel_init();
	ota_ab_init();

	crash_report_init();
	crash_alive_init();
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
	power_manager_set_display_busy_query(renderer_busy);
	power_manager_set_imu_reschedule_cb(imu_pipeline_reschedule);
	power_manager_init();
	battery_bench_init();
	radio_scheduler_init(ble_imu_gatt_set_traffic_paused);

	LOG_INF("stage: network manager");
	network_manager_init();
	ota_wifi_init();

	LOG_INF("%s — MCUboot A/B OTA ch=%s", FW_VERSION_NAME, ota_channel_get());
#if defined(CONFIG_REQUIRES_STACK_CANARIES)
	{
		extern volatile uintptr_t __stack_chk_guard;

		if (__stack_chk_guard == 0U) {
			LOG_ERR("stack_chk_guard is 0 — canaries useless / EXCM risk");
		} else {
			LOG_INF("stack_chk_guard ok (%p)", (void *)__stack_chk_guard);
		}
	}
#endif

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
	mt200_sample_queue_init();
	stall_watchdog_feed_main();

	power_manager_mark_ready();
	imu_pipeline_start();
#if defined(CONFIG_APP_CRASH_DEBUG)
	/* Real IMU score after deferred bring-up (early BIST treats who=0x00 as deferred). */
	bist_imu_finalize(3000);
#endif
	stall_watchdog_feed_main();

	k_thread_create(&render_thread, render_stack, K_THREAD_STACK_SIZEOF(render_stack),
			render_thread_fn, NULL, NULL, NULL, RENDER_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&render_thread, "render");

	LOG_INF("stage: main loop (BOOT tap = backlight toggle)");

	while (1) {
		STACK_RA_CHECK();
		const uint32_t now = k_uptime_get_32();

		g_main_loops++;
		if (boot_button_take_toggle_request()) {
			if (!battery_bench_config_locked()) {
				stall_watchdog_feed_main();
				MAIN_STEP("pm_toggle");
				power_manager_toggle_mode();
				stall_watchdog_feed_main();
			}
		}
		MAIN_STEP("boot_btn");
		boot_button_poll();
		STACK_RA_CHECK();
		MAIN_STEP("crash_dbg");
		crash_debug_poll();
		MAIN_STEP("ble_cfg");
		ble_config_gatt_poll();
		STACK_RA_CHECK();
		MAIN_STEP("ble_looper");
		ble_looper_poll();
		STACK_RA_CHECK();
		if (!ble_ota_ui_active()) {
			/*
			 * Never I2C-poll the QMI8658 while panel SPI is in flight.
			 * v310/v311: imu_poll concurrent with flush_y>=0 → LoadProhibited
			 * (EXCCAUSE 28 @ VADDR~0x2c) → DoubleException hang. Desk dump
			 * symbolicated into qmi8658_read/reg_read + fatal/log path.
			 */
			if (!renderer_busy()) {
				MAIN_STEP("imu_poll");
				imu_pipeline_poll();
			} else {
				MAIN_STEP("imu_hold_spi");
			}
		}
		STACK_RA_CHECK();
		MAIN_STEP("dev_cfg");
		device_config_poll();
		MAIN_STEP("ota_ab");
		ota_ab_poll();
		MAIN_STEP("ota_wifi");
		ota_wifi_poll();
		STACK_RA_CHECK();
		if (!ble_ota_ui_active()) {
			MAIN_STEP("ntp");
			clock_sync_ntp_poll();
		}
		if (!ble_ota_ui_active()) {
			MAIN_STEP("batt");
			battery_monitor_tick();
		}
		STACK_RA_CHECK();
		if (!ble_ota_ui_active() && battery_bench_safety_poll()) {
			MAIN_STEP("bench_safe");
			ble_imu_gatt_bench_sample();
		}
		MAIN_STEP("chip_temp");
		chip_temp_tick();
		MAIN_STEP("pm_tick");
		power_manager_tick();
		if (!ble_ota_ui_active() && battery_bench_tick(now)) {
			MAIN_STEP("bench_tick");
			ble_imu_gatt_bench_sample();
		}
		if (!ble_ota_ui_active()) {
			MAIN_STEP("net");
			network_manager_tick();
			MAIN_STEP("ble_net");
			ble_net_gatt_tick();
		}
		MAIN_STEP("ble_ota");
		ble_ota_gatt_tick();
		if (!ble_ota_ui_active()) {
			MAIN_STEP("vibro");
			vibro_capture_session_tick(now);
			vibro_capture_poll();
			vibro_led_poll();
		}
		{
			const struct device_config_v1 *dcfg = device_config_runtime();

			MAIN_STEP("radio_sched");
			radio_scheduler_set_capture_prep(
				vibro_schedule_capture_prep_active(dcfg, clock_sync_now_ms32()));
		}

		MAIN_STEP("wdt_feed");
		stall_watchdog_feed_main();
		if (!power_manager_tft_render_enabled()) {
			stall_watchdog_feed_render();
		}

		/* Stall breadcrumb every 5s — last step name survives if the next call wedges. */
		{
			static uint32_t last_step_hb_ms;

			if (last_step_hb_ms == 0U || (now - last_step_hb_ms) >= 5000U) {
				last_step_hb_ms = now;
				/* Feed WDT *before* USB printk — a wedged CDC must not starve soft WDT. */
				stall_watchdog_feed_main();
				/* printk first — survives USB CDC backpressure better than
				 * LOG_IMMEDIATE (and still useful under deferred logging). */
				printk("main_hb ok loops=%u step=%s render=%s phone=%u mt200=%u\n",
				       g_main_loops, g_main_step ? g_main_step : "?",
				       render_stage_name(g_render_stage),
				       ble_imu_link_active() ? 1U : 0U,
				       mt200_bridge_wanted() ? (mt200_bridge_radio_busy() ? 2U : 1U)
							     : 0U);
				/* printk is the soak heartbeat; skip duplicate LOG under IMMEDIATE. */
			}
		}

		if (last_hb_ms == 0U) {
			last_hb_ms = now;
		} else if (now - last_hb_ms >= 10000U) {
			MAIN_STEP("hb10");
			const uint32_t window = now - last_hb_ms;
			const bool screen_on = power_manager_tft_render_enabled();

			const uint32_t imu_ticks = imu_pipeline_take_hb_ticks();

			stall_watchdog_hb_window(hb_render_frames, screen_on, imu_ticks);
			crash_alive_mark_running(now, g_main_step ? (const char *)g_main_step : "?",
						 g_render_stage);

			if (screen_on && hb_render_frames == 0U) {
				const uint8_t stage = g_render_stage;
				char spi_info[64];

				(void)renderer_stall_info(spi_info, sizeof(spi_info));
				LOG_WRN("render stalled (%ums) — nudge panel on render thread "
					"(stuck@%s for %ums) spi: %s",
					window, render_stage_name(stage),
					now - g_render_stage_ms, spi_info);
				panel_backlight_reapply();
				power_manager_request_panel_hw(true);
			}

			/* OTA skips imu_pipeline_poll — do not treat that as a stall
			 * and kick recover/cal onto the IMU wq during flash erase. */
			if (screen_on && imu_ticks == 0U && power_manager_imu_hz_target() > 0U &&
			    !ble_ota_ui_active() && !imu_pipeline_recovering()) {
				LOG_WRN("IMU stalled (%ums) — recover", window);
				imu_pipeline_request_recover();
			}

			/* Load-shed pauses work; keep HB logs quiet too during OTA UI. */
			if (!ble_ota_ui_active()) {
				MAIN_STEP("telemetry");
				power_manager_log_telemetry(hb_render_frames, imu_ticks, window,
							    renderer_last_present_ms());
				imu_pipeline_log_motion();
			}
			hb_render_frames = 0;
			last_hb_ms = now;
		}

		MAIN_STEP("sleep");
		k_msleep(power_manager_main_sleep_ms());
		MAIN_STEP("loop");
	}

	return 0;
}
