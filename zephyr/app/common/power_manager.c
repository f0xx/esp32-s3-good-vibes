#include "power_manager.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/esp32_clock_control.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <hal/clk_tree_hal.h>

#include "battery_monitor.h"
#include "board_config.h"
#include "crash_ring_store.h"
#include "device_config.h"
#include "panel_backlight.h"
#include "stall_watchdog.h"

LOG_MODULE_REGISTER(power_mgr, LOG_LEVEL_INF);

static const struct device *g_display;
static struct device_config_v1 g_cfg;
static bool g_screen_on = true;
static bool g_tft_render = true;
static bool g_panel_hw_on = true;
static bool g_panel_hw_want = true;
static bool g_panel_hw_sync_needed;
static bool (*g_panel_hw_fn)(bool on);
static bool g_cpu_apply_pending;
static bool g_cpu_ready;
static bool g_ble_active;
static bool g_bt_controller_on;
static bool (*g_display_busy_query)(void);
static void (*g_imu_reschedule)(void);
static uint8_t g_cpu_settled_mhz = 240;
static uint8_t g_target_cpu_mhz = 240;
static uint8_t g_imu_hz = 10;
static uint8_t g_render_hz = RENDER_HZ_DEFAULT;
static uint8_t g_last_render_hz;
static uint8_t g_last_imu_hz;
static uint16_t g_last_bat_mv;
static uint8_t g_last_bat_pct;
static uint8_t g_last_power_profile;
static bool g_staging_mode;
static bool g_last_on_dc = true;
static bool g_last_on_dc_valid;

static uint8_t clamp_cpu_mhz(uint8_t mhz)
{
	if (mhz <= 80) {
		return 80;
	}
	if (mhz <= 160) {
		return 160;
	}
	return 240;
}

static int apply_cpu_mhz(uint8_t mhz)
{
#if DT_HAS_COMPAT_STATUS_OKAY(espressif_xtensa_lx7)
	static const struct device *clk_dev = DEVICE_DT_GET(DT_NODELABEL(rtc));
	struct esp32_clock_config clk_cfg = { 0 };
#endif
	int err = 0;

	mhz = clamp_cpu_mhz(mhz);

#if DT_HAS_COMPAT_STATUS_OKAY(espressif_xtensa_lx7)
	clk_cfg.cpu.clk_src = ESP32_CPU_CLK_SRC_PLL;
	clk_cfg.cpu.cpu_freq = mhz;
	err = clock_control_configure(
		clk_dev, (clock_control_subsys_t)ESP32_CLOCK_CONTROL_SUBSYS_CPU, &clk_cfg);
#endif

	if (err == 0) {
		g_cpu_settled_mhz = mhz;
	}
	return err;
}

static void apply_cpu_if_needed(uint8_t mhz)
{
	mhz = clamp_cpu_mhz(mhz);
	g_target_cpu_mhz = mhz;

	if (!g_cpu_ready) {
		return;
	}

	if (g_cpu_settled_mhz == mhz && power_manager_cpu_mhz_actual() == mhz) {
		return;
	}

	if (apply_cpu_mhz(mhz) == 0) {
		LOG_INF("CPU %u MHz (actual %u MHz)", g_cpu_settled_mhz,
			power_manager_cpu_mhz_actual());
	} else {
		LOG_WRN("CPU %u MHz failed (actual %u MHz)", mhz,
			power_manager_cpu_mhz_actual());
	}
}

static bool set_panel_hw(bool on)
{
	if (g_panel_hw_on == on) {
		return true;
	}

	if (g_panel_hw_fn != NULL && !g_panel_hw_fn(on)) {
		return false;
	}

	g_panel_hw_on = on;
	return true;
}

static void request_panel_hw(bool on)
{
	g_panel_hw_want = on;
	g_panel_hw_sync_needed = true;
}

void power_manager_request_panel_hw(bool on)
{
	request_panel_hw(on);
}

void power_manager_sync_panel_hw(void)
{
	if (!g_panel_hw_sync_needed && g_panel_hw_on == g_panel_hw_want) {
		return;
	}

	if (set_panel_hw(g_panel_hw_want)) {
		g_panel_hw_sync_needed = false;
	}
}

static bool display_is_busy(void)
{
	return g_display_busy_query != NULL && g_display_busy_query();
}

static void apply_cpu_deferred(void)
{
	if (!g_cpu_apply_pending || g_panel_hw_sync_needed || display_is_busy()) {
		return;
	}

	g_cpu_apply_pending = false;
	stall_watchdog_feed_main();
	apply_cpu_if_needed(g_target_cpu_mhz);
	stall_watchdog_feed_main();
}

void power_manager_sync_render(void)
{
	power_manager_sync_panel_hw();
}

void power_manager_set_display_busy_query(bool (*fn)(void))
{
	g_display_busy_query = fn;
}

/*
 * Demo/staging × DC/battery target matrix (BOOT tap cycles demo<->staging; DC/BAT
 * follows the live power source). g_screen_on is a separate manual override (phone
 * "toggle ESP screen" button / legacy behavior) that always wins over the mode default.
 */
static void apply_rates(void)
{
	const struct battery_state *bat = battery_monitor_state();
	const bool on_dc = (bat == NULL || !bat->valid) ? true : bat->on_dc;
	uint8_t target_cpu;
	uint8_t render_hz;
	bool want_panel;
	uint8_t imu_hz;

	if (!g_staging_mode) {
		/* Demo — always max IMU, backlight on; CPU/render halve on battery. */
		target_cpu = on_dc ? OPMODE_CPU_MHZ_DEMO_DC : OPMODE_CPU_MHZ_DEMO_BAT;
		render_hz = on_dc ? RENDER_HZ_DEFAULT : (uint8_t)(RENDER_HZ_DEFAULT / 2U);
		want_panel = true;
		imu_hz = IMU_SAMPLE_HZ_DEFAULT;
	} else if (on_dc) {
		/* Staging on DC — half speed, backlight stays on. */
		target_cpu = OPMODE_CPU_MHZ_STAGING_DC;
		render_hz = (uint8_t)(RENDER_HZ_DEFAULT / 2U);
		want_panel = true;
		imu_hz = IMU_SAMPLE_HZ_DEFAULT;
	} else {
		/* Staging on battery — minimum footprint: render + backlight off. IMU stays
		 * max only for low/ultra-low RPM diagnosis (tier 1/2), where each sparse
		 * capture window needs full fidelity; other tiers drop to the min rate. */
		const uint8_t vibro_tier = device_config_runtime()->vibro_capture_tier;

		target_cpu = OPMODE_CPU_MHZ_STAGING_BAT;
		render_hz = 0U;
		want_panel = false;
		/* Low-RPM (tier 1) / ultra-low-RPM (tier 2) diagnosis need full-rate IMU even
		 * on battery — their capture windows are sparse and rely on fidelity. Normal
		 * (0) and intermittent (3) fall back to the minimum rate to save power. */
		imu_hz = (vibro_tier == 1U || vibro_tier == 2U) ? IMU_SAMPLE_HZ_DEFAULT
								  : IMU_SAMPLE_HZ_MIN;
	}

	/* Keep 240 MHz while BLE is linked or controller is up — downclock after
	 * bt_enable can block clock_control for >15s (boot WDT / TG0WDT). */
	if (g_ble_active || g_bt_controller_on) {
		target_cpu = 240U;
	}

	g_target_cpu_mhz = clamp_cpu_mhz(target_cpu);
	g_imu_hz = imu_hz;
	g_render_hz = g_screen_on ? render_hz : 0U;
	g_tft_render = want_panel;
	request_panel_hw(want_panel && g_screen_on);

	g_cpu_apply_pending = true;
}

void power_manager_set_display(const struct device *display)
{
	g_display = display;
}

void power_manager_set_imu_reschedule_cb(void (*cb)(void))
{
	g_imu_reschedule = cb;
}

void power_manager_set_ble_active(bool active)
{
	if (g_ble_active == active) {
		return;
	}

	g_ble_active = active;
	if (!g_screen_on) {
		apply_rates();
	}
}

void power_manager_set_bt_controller_on(bool on)
{
	if (g_bt_controller_on == on) {
		return;
	}

	g_bt_controller_on = on;
	if (!g_screen_on) {
		apply_rates();
	}
}

void power_manager_set_panel_hw_fn(bool (*fn)(bool on))
{
	g_panel_hw_fn = fn;
}

void power_manager_init(void)
{
	device_config_load(&g_cfg);
	g_screen_on = !device_config_user_screen_off();
	g_staging_mode = device_config_staging_mode();
	if (!g_screen_on) {
		g_tft_render = false;
		g_render_hz = 0;
	}
	g_cpu_settled_mhz = power_manager_cpu_mhz_actual();
	g_target_cpu_mhz = 240;
	{
		const struct battery_state *bat = battery_monitor_state();

		g_last_on_dc = (bat == NULL || !bat->valid) ? true : bat->on_dc;
		g_last_on_dc_valid = true;
	}
	apply_rates();
	if (!g_screen_on) {
		panel_backlight_set_on(false);
	}
	/* Panel blanking deferred to render thread (no main-thread display mutex). */
	LOG_INF("power mode=%s cpu=%u MHz actual=%u apb=%u imu=%uHz render=%uHz",
		g_staging_mode ? "staging" : "demo", g_target_cpu_mhz,
		power_manager_cpu_mhz_actual(), power_manager_apb_mhz_actual(), g_imu_hz,
		g_render_hz);
}

bool power_manager_staging_mode(void)
{
	return g_staging_mode;
}

void power_manager_toggle_mode(void)
{
	g_staging_mode = !g_staging_mode;
	device_config_set_staging_mode(g_staging_mode);
	apply_rates();
	LOG_INF("BOOT tap → mode=%s cpu=%u MHz render=%uHz imu=%uHz backlight=%s",
		g_staging_mode ? "staging" : "demo", g_target_cpu_mhz, g_render_hz, g_imu_hz,
		g_panel_hw_want ? "on" : "off");
}

void power_manager_mark_ready(void)
{
	g_cpu_ready = true;
	if (g_cpu_settled_mhz != clamp_cpu_mhz(g_target_cpu_mhz) ||
	    power_manager_cpu_mhz_actual() != clamp_cpu_mhz(g_target_cpu_mhz)) {
		g_cpu_apply_pending = true;
	}
}

void power_manager_on_screen(bool on)
{
	if (g_screen_on == on) {
		return;
	}

	/* Stop render before touching panel HW (render thread may be mid-flush). */
	if (!on) {
		g_tft_render = false;
		g_render_hz = 0;
		panel_backlight_set_on(false);
	}

	g_screen_on = on;
	device_config_set_user_screen(on);
	battery_monitor_settle(1500U);
	apply_rates();
	if (g_imu_reschedule != NULL) {
		g_imu_reschedule();
	}
	LOG_INF("screen %s cpu=%u MHz render=%uHz imu=%uHz ble=%u", on ? "on" : "off",
		power_manager_cpu_mhz_actual(), g_render_hz, g_imu_hz, g_ble_active ? 1U : 0U);
}

void power_manager_reapply_backlight_if_screen_on(void)
{
	if (g_screen_on) {
		panel_backlight_reapply();
	}
}

void power_manager_tick(void)
{
	static uint32_t last_bl_ms;
	const struct battery_state *bat = battery_monitor_state();
	const bool on_dc = (bat == NULL || !bat->valid) ? g_last_on_dc : bat->on_dc;

	if (!g_last_on_dc_valid || on_dc != g_last_on_dc) {
		g_last_on_dc = on_dc;
		g_last_on_dc_valid = true;
		LOG_INF("power source -> %s — re-applying %s mode targets", on_dc ? "DC" : "BAT",
			g_staging_mode ? "staging" : "demo");
		apply_rates();
		if (g_imu_reschedule != NULL) {
			g_imu_reschedule();
		}
	}

	apply_cpu_deferred();

	if (!g_screen_on) {
		return;
	}

	const uint32_t now = k_uptime_get_32();

	if (last_bl_ms == 0U || (now - last_bl_ms) >= 5000U) {
		panel_backlight_reapply();
		last_bl_ms = now;
	}
}

bool power_manager_screen_on(void)
{
	return g_screen_on;
}

bool power_manager_tft_render_enabled(void)
{
	return g_tft_render && g_screen_on;
}

uint32_t power_manager_imu_interval_ms(void)
{
	return 1000U / g_imu_hz;
}

uint32_t power_manager_render_interval_ms(void)
{
	if (!power_manager_tft_render_enabled() || g_render_hz == 0) {
		return 500U;
	}
	return 1000U / g_render_hz;
}

uint8_t power_manager_cpu_mhz_settled(void)
{
	return g_cpu_settled_mhz;
}

uint8_t power_manager_cpu_mhz_target(void)
{
	return g_target_cpu_mhz;
}

uint8_t power_manager_cpu_mhz_actual(void)
{
	const uint32_t hz = clk_hal_cpu_get_freq_hz();

	return (uint8_t)(hz / 1000000U);
}

uint8_t power_manager_apb_mhz_actual(void)
{
	const uint32_t hz = clk_hal_apb_get_freq_hz();

	return (uint8_t)(hz / 1000000U);
}

uint8_t power_manager_render_hz_target(void)
{
	return g_render_hz;
}

uint8_t power_manager_imu_hz_target(void)
{
	return g_imu_hz;
}

void power_manager_log_telemetry(uint32_t render_frames, uint32_t imu_ticks,
				 uint32_t window_ms, uint32_t flush_ms)
{
	const uint32_t render_x10 = window_ms ? (render_frames * 10000U / window_ms) : 0;
	const uint32_t imu_x10 = window_ms ? (imu_ticks * 10000U / window_ms) : 0;
	const uint32_t frame_ms = render_frames ? (window_ms / render_frames) : 0U;
	const struct battery_state *bat = battery_monitor_state();

	LOG_INF("telemetry screen=%s target(cpu=%u render=%uHz imu=%uHz) "
		"actual(cpu=%u settled=%u apb=%u render=%u.%uHz imu=%u.%uHz frame=%ums flush=%ums "
		"bat=%.2fV %u%% adc=%umV src=%s",
		g_screen_on ? "on" : "off", g_target_cpu_mhz, g_render_hz, g_imu_hz,
		power_manager_cpu_mhz_actual(), g_cpu_settled_mhz,
		power_manager_apb_mhz_actual(), render_x10 / 10U, render_x10 % 10U,
		imu_x10 / 10U, imu_x10 % 10U, frame_ms, flush_ms,
		bat != NULL && bat->valid ? (double)bat->voltage_v : 0.0,
		bat != NULL ? bat->percent : 0U,
		bat != NULL ? bat->adc_mv : 0U,
		bat != NULL && bat->on_dc ? "DC" : "BAT");

	g_last_render_hz = (uint8_t)(render_x10 / 10U);
	g_last_imu_hz = (uint8_t)(imu_x10 / 10U);
	g_last_bat_mv = bat != NULL ? bat->adc_mv : 0U;
	g_last_bat_pct = bat != NULL ? bat->percent : 0U;
	g_last_power_profile = g_cfg.power_profile;
}

void power_manager_telemetry_snapshot(struct crash_ring_telemetry *out)
{
	if (out == NULL) {
		return;
	}

	out->render_hz = g_last_render_hz;
	out->imu_hz = g_last_imu_hz;
	out->bat_mv = g_last_bat_mv;
	out->bat_pct = g_last_bat_pct;
	out->power_profile = g_last_power_profile;
}

uint8_t power_manager_cpu_mhz(void)
{
	return g_cpu_settled_mhz;
}
