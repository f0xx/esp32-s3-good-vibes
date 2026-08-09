/*
 * Stall / task watchdog — step 3a: turn silent hangs into panics + coredump.
 */

#include "stall_watchdog.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/task_wdt/task_wdt.h>

LOG_MODULE_REGISTER(stall_wdt, LOG_LEVEL_INF);

#define MAIN_WDT_MS          15000U
#define RENDER_WDT_MS        12000U
#define RENDER_STALL_WINDOWS 2U

/* Overridden by handshake imu_pipeline.c when linked. */
__attribute__((weak)) bool imu_pipeline_recovering(void)
{
	return false;
}

__attribute__((weak)) bool imu_pipeline_live(void)
{
	return true;
}

static int g_main_wdt_ch = -1;
static int g_render_wdt_ch = -1;
static uint8_t g_render_stall_windows;

#if DT_NODE_HAS_STATUS(DT_ALIAS(watchdog0), okay)
#define WDT_NODE DT_ALIAS(watchdog0)
#else
#define WDT_NODE DT_INVALID_NODE
#endif

static void wdt_panic_cb(int channel_id, void *user_data)
{
	ARG_UNUSED(user_data);
	LOG_ERR("task WDT timeout ch=%d — panic for coredump", channel_id);
	k_panic();
}

int stall_watchdog_init(void)
{
	const struct device *hw_wdt = DEVICE_DT_GET_OR_NULL(WDT_NODE);
	int ret;

	if (hw_wdt != NULL && !device_is_ready(hw_wdt)) {
		LOG_WRN("HW watchdog not ready — task WDT only");
		hw_wdt = NULL;
	}

	ret = task_wdt_init(hw_wdt);
	if (ret != 0) {
		LOG_ERR("task_wdt_init failed (%d)", ret);
		return ret;
	}

	g_main_wdt_ch = task_wdt_add(MAIN_WDT_MS, wdt_panic_cb, (void *)"main");
	if (g_main_wdt_ch < 0) {
		LOG_ERR("task_wdt_add failed main=%d", g_main_wdt_ch);
		return -ENOMEM;
	}

	LOG_INF("stall watchdog ready (main=%ums render=%ums hw=%s)", MAIN_WDT_MS, RENDER_WDT_MS,
		hw_wdt != NULL ? "yes" : "no");
	return 0;
}

void stall_watchdog_arm_render(void)
{
	if (g_render_wdt_ch >= 0) {
		return;
	}

	g_render_wdt_ch = task_wdt_add(RENDER_WDT_MS, wdt_panic_cb, (void *)"render");
	if (g_render_wdt_ch < 0) {
		LOG_ERR("task_wdt_add render failed (%d)", g_render_wdt_ch);
	}
}

void stall_watchdog_feed_main(void)
{
	if (g_main_wdt_ch >= 0) {
		(void)task_wdt_feed(g_main_wdt_ch);
	}
}

void stall_watchdog_feed_render(void)
{
	if (g_render_wdt_ch >= 0) {
		(void)task_wdt_feed(g_render_wdt_ch);
	}
}

void stall_watchdog_hb_window(uint32_t render_frames, bool screen_on, uint32_t imu_ticks)
{
	if (!screen_on || imu_pipeline_recovering()) {
		g_render_stall_windows = 0U;
		return;
	}

	/* Do not panic on hb imu_ticks==0 — BLE/WiFi load can delay imu_work without
	 * a hardware fault (see main.c telemetry comment). IMU recover uses read
	 * fail-streak in imu_pipeline_tick only. */

	if (!imu_pipeline_live()) {
		g_render_stall_windows = 0U;
		return;
	}

	if (render_frames == 0U) {
		g_render_stall_windows++;
		LOG_WRN("render stalled (%u/%u windows)", (unsigned)g_render_stall_windows,
			(unsigned)RENDER_STALL_WINDOWS);
		if (g_render_stall_windows >= RENDER_STALL_WINDOWS) {
			LOG_ERR("render stall panic");
			k_panic();
		}
	} else {
		g_render_stall_windows = 0U;
	}
}
