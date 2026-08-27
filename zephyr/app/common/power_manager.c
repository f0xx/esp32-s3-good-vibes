#include "power_manager.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/esp32_clock_control.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <hal/clk_tree_hal.h>
#include <soc/rtc.h>

#include "battery_monitor.h"
#include "board_config.h"
#include "crash_ring_store.h"
#include "device_config.h"
#include "panel_backlight.h"
#include "stall_watchdog.h"

LOG_MODULE_REGISTER(power_mgr, LOG_LEVEL_INF);

/** After screen-on while BLE is connected, cap CPU/render briefly to avoid VHCI races. */
#define SCREEN_BLE_RAMP_MS 4000

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
static int64_t g_screen_ble_ramp_until;
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
static uint8_t g_cpu_mhz_override;
static uint8_t g_imu_hz_override;
static uint8_t g_cpu_desired_mhz = 240;
static bool g_cpu_ble_clamped;
static bool g_bench_lock;

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

/*
 * Fast, BT-safe CPU frequency switch.
 *
 * The straightforward path — Zephyr's clock_control_configure() -> esp32_cpu_clock_configure()
 * — unconditionally resets and re-enables the internal analog I2C bus (regi2c) shared with the
 * BT/WiFi PHY calibration hardware, then re-derives APB/UART timing from XTAL, on *every* call
 * regardless of target frequency. That reset is what wedged/crashed the board while BT was
 * active (see git history — TG0WDT_SYS_RST roughly every 37s).
 *
 * ESP-IDF's own esp_pm never goes through that path for live DFS while WiFi/BT is running.
 * It uses rtc_clk_cpu_freq_set_config_fast(), which — when both the current and target configs
 * are SOC_CPU_CLK_SRC_PLL (true for all of 80/160/240 MHz on this chip: all three are just
 * different dividers off the *same already-running* 480 MHz BBPLL) — only rewrites the CPU
 * clock divider and a couple of LDO/DBIAS regi2c fields (rtc_clk_cpu_freq_to_pll_mhz()), the
 * same register writes IDF performs continuously during BLE-active dynamic frequency scaling.
 * No bus reset, no BBPLL re-enable/reconfigure, no UART/APB re-derivation needed since APB stays
 * fixed at 80 MHz across all three PLL steps. We mirror esp_pm's portENTER_CRITICAL guard with a
 * short irq_lock() around the actual register writes (tens of microseconds, not a stall).
 *
 * Dropping to XTAL-sourced frequencies (<80 MHz, e.g. 40 MHz) is a different, riskier code path
 * (disables the BBPLL outright) and is intentionally not attempted here — see
 * power_manager_cpu_mhz_min_supported_while_bt_on().
 */
static int apply_cpu_mhz_fast(uint8_t mhz)
{
	rtc_cpu_freq_config_t old_config;
	rtc_cpu_freq_config_t new_config;

	rtc_clk_cpu_freq_get_config(&old_config);

	if (!rtc_clk_cpu_freq_mhz_to_config(mhz, &new_config)) {
		return -EINVAL;
	}
	if (old_config.source != SOC_CPU_CLK_SRC_PLL || new_config.source != SOC_CPU_CLK_SRC_PLL) {
		return -ENOTSUP;
	}

	unsigned int key = irq_lock();

	rtc_clk_cpu_freq_set_config_fast(&new_config);

	irq_unlock(key);

	return 0;
}

static int apply_cpu_mhz(uint8_t mhz)
{
	int err;

	mhz = clamp_cpu_mhz(mhz);

	err = apply_cpu_mhz_fast(mhz);
	if (err != 0) {
		/* Should not happen — clamp_cpu_mhz() only ever returns 80/160/240, all PLL —
		 * but fall back to the full (slower, BT-unsafe) driver path rather than silently
		 * doing nothing if some future tier ever asks for something else. */
		LOG_WRN("CPU fast path unavailable (%d) — falling back to full reconfigure", err);
#if DT_HAS_COMPAT_STATUS_OKAY(espressif_xtensa_lx7)
		static const struct device *clk_dev = DEVICE_DT_GET(DT_NODELABEL(rtc));
		struct esp32_clock_config clk_cfg = { 0 };

		clk_cfg.cpu.clk_src = ESP32_CPU_CLK_SRC_PLL;
		clk_cfg.cpu.cpu_freq = mhz;
		err = clock_control_configure(
			clk_dev, (clock_control_subsys_t)ESP32_CLOCK_CONTROL_SUBSYS_CPU, &clk_cfg);
#endif
	}

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

static bool vibro_needs_full_imu(void)
{
	const uint8_t tier = device_config_runtime()->vibro_capture_tier;

	return tier == 1U || tier == 2U;
}

static uint8_t clamp_imu_hz(uint8_t hz)
{
	if (hz <= IMU_SAMPLE_HZ_MIN) {
		return IMU_SAMPLE_HZ_MIN;
	}
	if (hz >= IMU_SAMPLE_HZ_DEFAULT) {
		return IMU_SAMPLE_HZ_DEFAULT;
	}
	return hz;
}

/*
 * Screen-off awake tier: drop IMU/CPU even in demo mode. Full-rate IMU + 240 MHz demo
 * CPU made sense for the live scene, but with the panel off the phone only needs sparse
 * motion/telemetry — 100 Hz I2C polling was the dominant heat/current draw the user saw.
 */
static void apply_screen_off_idle_tiers(uint8_t *target_cpu, uint8_t *imu_hz)
{
	const struct device_config_v1 *cfg = device_config_runtime();

	if (g_screen_on || vibro_needs_full_imu()) {
		return;
	}

	if (*imu_hz > IMU_SAMPLE_HZ_IDLE) {
		*imu_hz = IMU_SAMPLE_HZ_IDLE;
	}
	if (cfg->imu_sample_hz >= IMU_SAMPLE_HZ_MIN && *imu_hz > cfg->imu_sample_hz) {
		*imu_hz = clamp_imu_hz(cfg->imu_sample_hz);
	}
	if (g_cpu_mhz_override == 0U && *target_cpu > OPMODE_CPU_MHZ_STAGING_BAT) {
		*target_cpu = OPMODE_CPU_MHZ_STAGING_BAT;
	}
	if (g_cpu_mhz_override == 0U && cfg->cpu_mhz >= 80U &&
	    *target_cpu > clamp_cpu_mhz(cfg->cpu_mhz)) {
		*target_cpu = clamp_cpu_mhz(cfg->cpu_mhz);
	}
}

/*
 * Demo/staging × DC/battery target matrix (BOOT tap cycles demo<->staging; DC/BAT
 * follows the live power source). g_screen_on is a separate manual override (phone
 * "toggle ESP screen" button / legacy behavior) that always wins over the mode default.
 */
static void apply_rates(void)
{
	if (g_bench_lock) {
		return;
	}

	const struct battery_state *bat = battery_monitor_state();
	const bool on_dc = (bat == NULL || !bat->valid) ? true : bat->on_dc;
	uint8_t target_cpu;
	uint8_t render_hz;
	bool want_panel;
	uint8_t imu_hz;

	if (!g_staging_mode) {
		/* Demo — full scene rates when the panel is on; screen-off idle tier below. */
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
		/* Staging on battery — minimum footprint by default: render + backlight off.
		 * IMU stays max only for low/ultra-low RPM diagnosis (tier 1/2), where each
		 * sparse capture window needs full fidelity; other tiers drop to the min rate.
		 *
		 * want_panel used to be hard-coded false here, which silently broke the
		 * documented "g_screen_on always wins over the mode default" contract below:
		 * request_panel_hw(want_panel && g_screen_on) can only ever turn things OFF
		 * when want_panel is false, so a user forcing the screen on via BLE while the
		 * device sat in staging+battery had zero effect on the physical backlight
		 * ("turn ESP display on" looked like a no-op). Deriving want_panel from
		 * g_screen_on restores the override: default stays off for power saving, but
		 * an explicit on request actually lights the panel (at a reduced render rate,
		 * matching staging-on-DC) instead of being masked out below.
		 */
		const uint8_t vibro_tier = device_config_runtime()->vibro_capture_tier;

		target_cpu = OPMODE_CPU_MHZ_STAGING_BAT;
		render_hz = g_screen_on ? (uint8_t)(RENDER_HZ_DEFAULT / 2U) : 0U;
		want_panel = g_screen_on;
		/* Low-RPM (tier 1) / ultra-low-RPM (tier 2) diagnosis need full-rate IMU even
		 * on battery — their capture windows are sparse and rely on fidelity. Normal
		 * (0) and intermittent (3) fall back to the minimum rate to save power. */
		imu_hz = (vibro_tier == 1U || vibro_tier == 2U) ? IMU_SAMPLE_HZ_DEFAULT
								  : IMU_SAMPLE_HZ_MIN;
	}

	apply_screen_off_idle_tiers(&target_cpu, &imu_hz);

	/* Manual overrides win over the demo/staging/battery matrix above (but not over the
	 * BLE-settle safety floor just below — see its comment). 0 = auto/no override. */
	if (g_cpu_mhz_override != 0U) {
		target_cpu = g_cpu_mhz_override;
	}
	if (g_imu_hz_override != 0U) {
		imu_hz = g_imu_hz_override;
	}

	if (g_screen_on && g_ble_active && g_screen_ble_ramp_until > 0 &&
	    k_uptime_get() < g_screen_ble_ramp_until) {
		if (target_cpu > OPMODE_CPU_MHZ_DEMO_BAT) {
			target_cpu = OPMODE_CPU_MHZ_DEMO_BAT;
		}
		if (render_hz > (RENDER_HZ_DEFAULT / 4U)) {
			render_hz = (uint8_t)(RENDER_HZ_DEFAULT / 4U);
		}
	}

	/*
	 * Historical note: this used to force a permanent 240 MHz floor for as long as the BT
	 * controller was on, because Zephyr's clock_control_configure() -> full
	 * esp32_cpu_clock_configure() resets the shared analog-I2C (regi2c) bus used by BT/WiFi
	 * PHY calibration on *every* call, which reliably wedged long enough to trip the
	 * hardware watchdog while BT was active (TG0WDT_SYS_RST every ~37s).
	 *
	 * apply_cpu_mhz_fast() (see apply_cpu_mhz() above) sidesteps that entirely for 80/160/
	 * 240 MHz: all three are just divider taps on the *same already-running* 480 MHz BBPLL,
	 * so switching between them only rewrites the CPU divider + a couple of LDO/DBIAS
	 * regi2c fields under a short irq_lock() — the exact mechanism ESP-IDF's own esp_pm
	 * uses for live dynamic frequency scaling *while WiFi/BT is running*. No bus reset, no
	 * BBPLL reconfigure. Verified stable across multi-minute BLE-connected sessions with
	 * live overrides between all three tiers (see ROADMAP.md).
	 *
	 * g_cpu_ble_clamped is kept only for the case a future tier asks for a sub-80 MHz
	 * (XTAL-sourced) frequency while BT is on — that path *does* still require disabling
	 * the BBPLL and is not attempted live (see power_manager_cpu_mhz_min_supported()).
	 */
	g_cpu_desired_mhz = clamp_cpu_mhz(target_cpu);
	g_cpu_ble_clamped = false;

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
	/* Always re-apply — demo/staging tiers can downclock below 240 MHz even with the
	 * screen on, so the BLE-linked 240 MHz floor (connection-settle stall guard)
	 * must be (re)armed/released regardless of g_screen_on. */
	apply_rates();
}

void power_manager_set_bt_controller_on(bool on)
{
	if (g_bt_controller_on == on) {
		return;
	}

	g_bt_controller_on = on;
	apply_rates();
}

bool power_manager_bench_locked(void)
{
	return g_bench_lock;
}

void power_manager_set_bench_lock(bool lock)
{
	if (g_bench_lock == lock) {
		return;
	}
	g_bench_lock = lock;
	if (!lock) {
		apply_rates();
	}
	LOG_INF("bench lock -> %s", lock ? "ON" : "OFF");
}

void power_manager_set_cpu_mhz_override(uint8_t mhz)
{
	if (g_bench_lock) {
		return;
	}
	if (g_cpu_mhz_override == mhz) {
		return;
	}

	g_cpu_mhz_override = mhz;
	device_config_set_cpu_mhz_override(mhz);
	apply_rates();
	LOG_INF("CPU MHz override -> %s", mhz != 0U ? "manual" : "auto");
}

uint8_t power_manager_cpu_mhz_override(void)
{
	return g_cpu_mhz_override;
}

void power_manager_set_imu_hz_override(uint8_t hz)
{
	if (g_bench_lock) {
		return;
	}
	if (g_imu_hz_override == hz) {
		return;
	}

	g_imu_hz_override = hz;
	device_config_set_imu_hz_override(hz);
	apply_rates();
	if (g_imu_reschedule != NULL) {
		g_imu_reschedule();
	}
	LOG_INF("IMU Hz override -> %s", hz != 0U ? "manual" : "auto");
}

uint8_t power_manager_imu_hz_override(void)
{
	return g_imu_hz_override;
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
	g_cpu_mhz_override = device_config_cpu_mhz_override();
	g_imu_hz_override = device_config_imu_hz_override();
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
	if (g_bench_lock) {
		return;
	}
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
	if (g_bench_lock) {
		return;
	}
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
	if (on && g_ble_active) {
		g_screen_ble_ramp_until = k_uptime_get() + (int64_t)SCREEN_BLE_RAMP_MS;
	} else if (!on) {
		g_screen_ble_ramp_until = 0;
	}
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
	/* Called right after BT/WiFi radio bring-up, which is exactly when the LEDC channel
	 * backing the backlight PWM can get silently reset. panel_backlight_reapply() now
	 * re-asserts whichever duty matches the current g_on state (on-percent or zero), so
	 * this must run unconditionally — previously it only reinforced the "on" case, which
	 * meant an off screen could get relit by radio init with nothing to correct it. */
	panel_backlight_reapply();
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

	if (g_screen_ble_ramp_until > 0 && k_uptime_get() >= g_screen_ble_ramp_until) {
		g_screen_ble_ramp_until = 0;
		apply_rates();
		if (g_imu_reschedule != NULL) {
			g_imu_reschedule();
		}
	}

	/* Reapply runs regardless of g_screen_on now — panel_backlight_reapply() re-asserts
	 * whichever duty (on-percent or zero) matches the current g_on state, guarding against
	 * a later BT/WiFi radio init silently resetting the LEDC channel in either direction. */
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

uint8_t power_manager_cpu_mhz_desired(void)
{
	return g_cpu_desired_mhz;
}

bool power_manager_cpu_ble_clamped(void)
{
	return g_cpu_ble_clamped;
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

uint32_t power_manager_main_sleep_ms(void)
{
	uint32_t sleep_ms = 10U;

	if (!power_manager_tft_render_enabled() && g_imu_hz > 0U) {
		const uint32_t imu_ms = power_manager_imu_interval_ms();

		if (imu_ms > sleep_ms) {
			sleep_ms = imu_ms / 2U;
			if (sleep_ms > 50U) {
				sleep_ms = 50U;
			}
		}
	}

	return sleep_ms;
}

void power_manager_log_telemetry(uint32_t render_frames, uint32_t imu_ticks,
				 uint32_t window_ms, uint32_t flush_ms)
{
	const uint32_t render_x10 = window_ms ? (render_frames * 10000U / window_ms) : 0;
	const uint32_t imu_x10 = window_ms ? (imu_ticks * 10000U / window_ms) : 0;
	const uint32_t frame_ms = render_frames ? (window_ms / render_frames) : 0U;
	const struct battery_state *bat = battery_monitor_state();

	LOG_INF("telemetry screen=%s target(cpu=%u want=%u%s render=%uHz imu=%uHz) "
		"actual(cpu=%u settled=%u apb=%u render=%u.%uHz imu=%u.%uHz frame=%ums flush=%ums "
		"bat=%.2fV %u%% adc=%umV src=%s",
		g_screen_on ? "on" : "off", g_target_cpu_mhz, g_cpu_desired_mhz,
		g_cpu_ble_clamped ? " BLE-clamp" : "", g_render_hz, g_imu_hz,
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
	g_last_power_profile = device_config_runtime()->power_profile;
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
