/*
 * Stall / task watchdog — turn silent hangs into a clean reboot + RTC capture.
 *
 * Do NOT k_panic() from the WDT callback: that runs coredump/logging and overwrites
 * EPC with coredump_logging_backend_* frames, which is what Issues showed as "PC".
 * Capture the starved *channel* name (main/render) into RTC, then sys_reboot.
 *
 * Desk (prj_crash): CONFIG_APP_STALL_WATCHDOG=n — hang-hunt via OpenOCD/GDB.
 * Stable OTA (prj_release): kept on.
 */

#include "stall_watchdog.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>

#include "crash_alive.h"
#include "crash_rtc_capture.h"
#include "radio_scheduler.h"
#include "renderer.h"

LOG_MODULE_REGISTER(stall_wdt, LOG_LEVEL_INF);

#if IS_ENABLED(CONFIG_APP_STALL_WATCHDOG)

#include <zephyr/task_wdt/task_wdt.h>

/*
 * Soft channel periods. Do not use CONFIG_TASK_WDT_HW_FALLBACK on ESP32 —
 * Zephyr's MWDT driver mistimes ms vs ticks and caused ~13.5s TG0 resets.
 */
#define MAIN_WDT_MS          25000U
#define RENDER_WDT_MS        20000U
#define RENDER_STALL_WINDOWS 3U
#define RENDER_ARM_MIN_FEEDS 3U

/* Overridden by handshake imu_pipeline.c when linked. */
__attribute__((weak)) bool imu_pipeline_recovering(void)
{
	return false;
}

__attribute__((weak)) bool imu_pipeline_live(void)
{
	return true;
}

static volatile int g_main_wdt_ch = -1;
static volatile int g_render_wdt_ch = -1;
static volatile uint8_t g_render_stall_windows;
static volatile uint8_t g_render_feed_count;
static volatile bool g_render_arm_requested;

static void wdt_timeout_cb(int channel_id, void *user_data)
{
	const char *channel = (user_data != NULL) ? (const char *)user_data : "wdt";

	/* Never LOG_* here: LOG_MODE_IMMEDIATE + a wedged UART/SPI path can block forever
	 * inside the timeout CB and prevent sys_reboot — matching "silent hang, no WDT line". */
	printk("stall_wdt: timeout ch=%d feeder=%s — RTC + reboot\n", channel_id, channel);
	crash_rtc_capture_task_wdt(channel);
	/* Keep alive dirty — boot uses RTC pending, not silent_hang. */
	sys_reboot(SYS_REBOOT_COLD);
}

int stall_watchdog_init(void)
{
	int ret;

	/*
	 * Never pass the ESP32 MWDT into task_wdt when HW fallback is enabled:
	 * zephyr/drivers/watchdog/wdt_esp32.c feeds window.max (ms) to
	 * wdt_hal_config_stage() as raw ticks. That made TG0WDT_SYS_RST fire
	 * around ~13.5s uptime in the field. Soft channels only.
	 */
	ret = task_wdt_init(NULL);
	if (ret != 0) {
		LOG_ERR("task_wdt_init failed (%d)", ret);
		return ret;
	}

	g_main_wdt_ch = task_wdt_add(MAIN_WDT_MS, wdt_timeout_cb, (void *)"main");
	if (g_main_wdt_ch < 0) {
		LOG_ERR("task_wdt_add failed main=%d", g_main_wdt_ch);
		return -ENOMEM;
	}

	LOG_INF("stall watchdog ready (main=%ums render=%ums arm_after=%u feeds hw=no)",
		MAIN_WDT_MS, RENDER_WDT_MS, (unsigned)RENDER_ARM_MIN_FEEDS);
	return 0;
}

void stall_watchdog_arm_render(void)
{
	/* Defer real arm until a few successful feeds — boot+phone+MT200 used to expire
	 * the render channel at ~13.5s before the first flush ever completed. */
	g_render_arm_requested = true;
}

static void arm_render_channel_if_ready(void)
{
	if (g_render_wdt_ch >= 0 || !g_render_arm_requested) {
		return;
	}
	if (g_render_feed_count < RENDER_ARM_MIN_FEEDS) {
		return;
	}

	g_render_wdt_ch = task_wdt_add(RENDER_WDT_MS, wdt_timeout_cb, (void *)"render");
	if (g_render_wdt_ch < 0) {
		LOG_ERR("task_wdt_add render failed (%d)", g_render_wdt_ch);
		return;
	}
	LOG_INF("render WDT armed after %u feeds (timeout=%ums)",
		(unsigned)g_render_feed_count, RENDER_WDT_MS);
}

void stall_watchdog_feed_main(void)
{
	if (g_main_wdt_ch >= 0) {
		(void)task_wdt_feed(g_main_wdt_ch);
	}
}

void stall_watchdog_feed_render(void)
{
	if (g_render_feed_count < 255U) {
		g_render_feed_count++;
	}
	arm_render_channel_if_ready();
	if (g_render_wdt_ch >= 0) {
		(void)task_wdt_feed(g_render_wdt_ch);
	}
}

void stall_watchdog_hb_window(uint32_t render_frames, bool screen_on, uint32_t imu_ticks)
{
	ARG_UNUSED(imu_ticks);

	if (!screen_on || imu_pipeline_recovering()) {
		g_render_stall_windows = 0U;
		return;
	}

	if (!imu_pipeline_live()) {
		g_render_stall_windows = 0U;
		return;
	}

	/*
	 * frames==0 is intentional while panel SPI is held (MT200 epoch) or soft-tripped
	 * after display_write -ETIMEDOUT. Capturing render-hb then mislabels a healthy
	 * feed path (PC often symbolicates near stall_watchdog_feed_render).
	 */
	if (!radio_scheduler_panel_spi_allowed() || renderer_spi_tripped()) {
		g_render_stall_windows = 0U;
		return;
	}

	if (render_frames == 0U) {
		g_render_stall_windows++;
		LOG_WRN("render stalled (%u/%u windows)", (unsigned)g_render_stall_windows,
			(unsigned)RENDER_STALL_WINDOWS);
		if (g_render_stall_windows >= RENDER_STALL_WINDOWS) {
			/*
			 * Do NOT cold-reboot from the heartbeat path. Under BLE/WiFi
			 * pressure a slow (but live) flush used to trip this and reset
			 * before the real render fault (EXCCAUSE 28 / Invalid SP) could
			 * surface. Soft task-WDT still reboots on true feed starvation.
			 */
			LOG_ERR("render stall — RTC capture only (no HB reboot)");
			crash_rtc_capture_task_wdt("render-hb");
			g_render_stall_windows = 0U;
		}
	} else {
		g_render_stall_windows = 0U;
	}
}

#else /* !CONFIG_APP_STALL_WATCHDOG */

int stall_watchdog_init(void)
{
	LOG_INF("stall watchdog disabled (desk hang-hunt)");
	return 0;
}

void stall_watchdog_arm_render(void)
{
}

void stall_watchdog_feed_main(void)
{
}

void stall_watchdog_feed_render(void)
{
}

void stall_watchdog_hb_window(uint32_t render_frames, bool screen_on, uint32_t imu_ticks)
{
	ARG_UNUSED(render_frames);
	ARG_UNUSED(screen_on);
	ARG_UNUSED(imu_ticks);
}

#endif /* CONFIG_APP_STALL_WATCHDOG */
