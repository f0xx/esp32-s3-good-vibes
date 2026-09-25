#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "device_config.h"
#include "floor_calib.h"
#include "host_debug.h"
#include "host_latency.h"
#include "imu_sample.h"
#include "power_manager.h"
#include "renderer.h"
#include "scene_zoom.h"
#include "stall_watchdog.h"
#include "vibro_capture.h"

/* Firmware crash_alive — no-op on host. */
void crash_alive_mark_running(uint32_t uptime_ms)
{
	(void)uptime_ms;
}
void crash_alive_mark_clean_shutdown(void)
{
}
bool crash_alive_dirty(void)
{
	return false;
}
uint32_t crash_alive_last_uptime_ms(void)
{
	return 0U;
}
void crash_alive_consume(void)
{
}

LOG_MODULE_REGISTER(stubs_host, LOG_LEVEL_INF);

/* --- device_config --- */

static struct device_config_v1 g_cfg;

static void cfg_defaults(void)
{
	memset(&g_cfg, 0, sizeof(g_cfg));
	g_cfg.magic = DEVICE_CONFIG_MAGIC;
	g_cfg.version = DEVICE_CONFIG_VERSION;
	g_cfg.size = (uint16_t)sizeof(g_cfg);
	g_cfg.walk_height_m = 1.80f;
	g_cfg.walk_pocket_m = 1.50f;
	g_cfg.walk_step_min_m = 0.70f;
	g_cfg.walk_step_max_m = 0.90f;
	g_cfg.imu_accel_scale = 1.0f;
	g_cfg.imu_gyro_scale = 1.0f;
	g_cfg.imu_sample_hz = 100;
	g_cfg.cpu_mhz = 240;
	LOG_DBG("device_config defaults applied");
}

const struct device_config_v1 *device_config_runtime(void)
{
	if (g_cfg.magic != DEVICE_CONFIG_MAGIC) {
		cfg_defaults();
	}
	return &g_cfg;
}

bool device_config_load(struct device_config_v1 *cfg)
{
	LOG_INF("device_config_load");
	cfg_defaults();
	if (cfg) {
		*cfg = g_cfg;
	}
	return true;
}

/* --- power_manager --- */

uint32_t power_manager_imu_interval_ms(void)
{
	return 10;
}

uint8_t power_manager_imu_hz_target(void)
{
	return 100;
}

void power_manager_init(void)
{
	LOG_INF("power_manager_init (host stub)");
}
void power_manager_mark_ready(void)
{
	LOG_DBG("power_manager_mark_ready");
}
void power_manager_tick(void)
{
	HOST_LOG(HOST_LOG_TRACE, "stubs_host", "power_manager_tick");
}

/* --- floor_calib --- */

void floor_calib_init(void)
{
	LOG_DBG("floor_calib_init");
}
void floor_calib_poll(void)
{
}
void floor_calib_feed(const struct imu_sample *sample)
{
	(void)sample;
}
void floor_calib_apply(struct imu_sample *sample)
{
	(void)sample;
}
bool floor_calib_valid(void)
{
	return false;
}

/* --- vibro --- */

void vibro_capture_init(void)
{
	LOG_DBG("vibro_capture_init");
}
void vibro_capture_apply_config(const struct device_config_v1 *cfg)
{
	LOG_DBG("vibro_capture_apply_config tier=%u", cfg ? cfg->vibro_capture_tier : 0);
}
void vibro_capture_push(const struct imu_sample *sample)
{
	(void)sample;
	HOST_LOG(HOST_LOG_TRACE, "stubs_host", "vibro_capture_push");
}

/* --- scene_zoom --- */

static float g_zoom[3] = { SCENE_ZOOM_DEFAULT, SCENE_ZOOM_DEFAULT, SCENE_ZOOM_DEFAULT };

void scene_zoom_init(void)
{
	LOG_DBG("scene_zoom_init default=%.2f", (double)SCENE_ZOOM_DEFAULT);
}
void scene_zoom_tick(const struct imu_sample *sample)
{
	(void)sample;
}
const float *scene_zoom_current(void)
{
	return g_zoom;
}

/* --- stall_watchdog --- */

int stall_watchdog_init(void)
{
	LOG_INF("stall_watchdog_init (host no-op)");
	return 0;
}
void stall_watchdog_arm_render(void)
{
	LOG_DBG("stall_watchdog_arm_render");
}
void stall_watchdog_feed_main(void)
{
	HOST_LOG(HOST_LOG_TRACE, "stubs_host", "stall_watchdog_feed_main");
}
void stall_watchdog_feed_render(void)
{
	HOST_LOG(HOST_LOG_TRACE, "stubs_host", "stall_watchdog_feed_render");
}
void stall_watchdog_hb_window(uint32_t render_frames, bool screen_on, uint32_t imu_ticks)
{
	LOG_DBG("stall_hb render=%u screen=%d imu=%u", render_frames, screen_on ? 1 : 0,
		imu_ticks);
}

/*
 * Desk gate: render holds g_ext_bus across display_write and sets
 * renderer_busy for the whole flush. IMU I2C uses try and skips.
 */
K_MUTEX_DEFINE(g_ext_bus);
static atomic_bool g_display_busy;

void renderer_ext_bus_lock(void)
{
	(void)k_mutex_lock(&g_ext_bus, K_FOREVER);
}

void renderer_ext_bus_unlock(void)
{
	k_mutex_unlock(&g_ext_bus);
}

bool renderer_ext_bus_try(void)
{
	return k_mutex_lock(&g_ext_bus, K_NO_WAIT) == 0;
}

bool renderer_busy(void)
{
	return atomic_load(&g_display_busy);
}

void host_panel_flush(void)
{
	atomic_store(&g_display_busy, true);
	renderer_ext_bus_lock();
	host_latency_us(HOST_LAT_RENDER_FLUSH_US);
	renderer_ext_bus_unlock();
	atomic_store(&g_display_busy, false);
}
