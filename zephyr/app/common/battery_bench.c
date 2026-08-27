#include "battery_bench.h"

#include <errno.h>
#include <string.h>

#include <esp_system.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>

#include "battery_monitor.h"
#include "board_config.h"
#include "power_manager.h"

LOG_MODULE_REGISTER(bat_bench, LOG_LEVEL_INF);

#define BENCH_SETTINGS_KEY "bench/state"
#define BENCH_MAGIC        0x42454E43U /* BENC */
/** Consecutive low-V / DC samples before auto-stop (filters ADC noise). */
#define BENCH_LOW_STREAK_STOP  3U
#define BENCH_DC_STREAK_STOP   2U

struct battery_bench_nvs {
	uint32_t magic;
	uint8_t active;
	uint8_t pad[3];
	uint32_t session_id;
	uint32_t sample_seq;
	uint32_t started_uptime_ms;
};

static struct battery_bench_nvs g_nvs;
static uint32_t g_last_sample_ms;
static bool g_loaded;
static bool g_boot_resume_checked;
static uint8_t g_low_v_streak;
static uint8_t g_dc_streak;

static int persist_nvs(void)
{
	return settings_save_one(BENCH_SETTINGS_KEY, &g_nvs, sizeof(g_nvs));
}

static int load_nvs(void)
{
	(void)settings_load_subtree("bench");
	if (g_nvs.magic != BENCH_MAGIC) {
		memset(&g_nvs, 0, sizeof(g_nvs));
		return -ENOENT;
	}
	return 0;
}

static int bench_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(name, "state") != 0) {
		return -ENOENT;
	}
	if (len != sizeof(g_nvs) || read_cb == NULL) {
		return -EINVAL;
	}
	if (read_cb(cb_arg, &g_nvs, len) != (ssize_t)len) {
		return -EIO;
	}
	if (g_nvs.magic != BENCH_MAGIC) {
		return -EINVAL;
	}
	return 0;
}

static uint32_t mint_session_id(void)
{
	uint32_t id = k_uptime_get_32() ^ sys_rand32_get();

	if (id == 0U) {
		id = 1U;
	}
	return id;
}

static int disarm_bench(void)
{
	g_nvs.active = 0U;
	g_nvs.magic = BENCH_MAGIC;
	g_low_v_streak = 0U;
	g_dc_streak = 0U;
	power_manager_set_bench_lock(false);
	if (persist_nvs() != 0) {
		LOG_WRN("bench NVS save failed");
	}
	LOG_INF("bench STOP sid=%u final_seq=%u", g_nvs.session_id, g_nvs.sample_seq);
	return 0;
}

static void disarm_bench_auto(const char *reason)
{
	const uint32_t sid = g_nvs.session_id;
	const uint32_t seq = g_nvs.sample_seq;

	(void)disarm_bench();
	LOG_WRN("bench AUTO-STOP (%s) sid=%u seq=%u", reason, sid, seq);
}

static bool bench_power_ok(bool for_resume)
{
	const struct battery_state *bat = battery_monitor_state();
	const float min_v = for_resume ? BATTERY_BENCH_RESUME_MIN_V : BATTERY_BENCH_STOP_V;

	if (bat == NULL || !bat->valid) {
		return !for_resume;
	}
	if (bat->on_dc) {
		return false;
	}
	return bat->voltage_v >= min_v;
}

static void abort_unsafe_resume(void)
{
	if (g_nvs.active == 0U) {
		return;
	}

	if (esp_reset_reason() == ESP_RST_BROWNOUT) {
		disarm_bench_auto("brownout reset");
		return;
	}

	if (!bench_power_ok(true)) {
		const struct battery_state *bat = battery_monitor_state();

		if (bat != NULL && bat->valid) {
			LOG_WRN("bench resume blocked — V=%.2f src=%s (need V>=%.2f on BAT)",
				(double)bat->voltage_v, bat->on_dc ? "DC" : "BAT",
				(double)BATTERY_BENCH_RESUME_MIN_V);
			disarm_bench_auto("unsafe power at boot");
		}
	}
}

static int arm_bench(bool resume)
{
	if (!resume && !bench_power_ok(false)) {
		const struct battery_state *bat = battery_monitor_state();

		if (bat != NULL && bat->valid) {
			LOG_WRN("bench START rejected — V=%.2f src=%s", (double)bat->voltage_v,
				bat->on_dc ? "DC" : "BAT");
		} else {
			LOG_WRN("bench START rejected — battery not valid");
		}
		return -EINVAL;
	}

	if (!resume) {
		g_nvs.session_id = mint_session_id();
		g_nvs.sample_seq = 0U;
		g_nvs.started_uptime_ms = k_uptime_get_32();
	}
	g_nvs.magic = BENCH_MAGIC;
	g_nvs.active = 1U;
	g_last_sample_ms = 0U;
	g_low_v_streak = 0U;
	g_dc_streak = 0U;
	power_manager_set_bench_lock(true);
	if (persist_nvs() != 0) {
		LOG_WRN("bench NVS save failed");
	}
	LOG_INF("bench START sid=%u seq=%u resume=%d", g_nvs.session_id, g_nvs.sample_seq, resume);
	return 0;
}

int battery_bench_init(void)
{
	if (g_loaded) {
		return 0;
	}
	g_loaded = true;
	if (load_nvs() != 0) {
		return 0;
	}
	if (g_nvs.active != 0U) {
		if (esp_reset_reason() == ESP_RST_BROWNOUT) {
			disarm_bench_auto("brownout reset");
			return 0;
		}
		LOG_INF("bench resume sid=%u seq=%u (power check deferred)", g_nvs.session_id,
			g_nvs.sample_seq);
		power_manager_set_bench_lock(true);
	}
	return 0;
}

bool battery_bench_active(void)
{
	return g_nvs.active != 0U;
}

bool battery_bench_config_locked(void)
{
	return battery_bench_active();
}

uint32_t battery_bench_session_id(void)
{
	return g_nvs.session_id;
}

uint32_t battery_bench_sample_seq(void)
{
	return g_nvs.sample_seq;
}

int battery_bench_start(void)
{
	if (g_nvs.active != 0U) {
		return -EALREADY;
	}
	return arm_bench(false);
}

int battery_bench_stop(void)
{
	if (g_nvs.active == 0U) {
		return -EALREADY;
	}
	return disarm_bench();
}

bool battery_bench_tick(uint32_t now_ms)
{
	if (g_nvs.active == 0U) {
		return false;
	}
	if (g_last_sample_ms != 0U && (now_ms - g_last_sample_ms) < BATTERY_BENCH_SAMPLE_MS) {
		return false;
	}
	g_last_sample_ms = now_ms;
	g_nvs.sample_seq++;
	(void)persist_nvs();
	return true;
}

bool battery_bench_safety_poll(void)
{
	const bool was_active = g_nvs.active != 0U;

	if (!g_boot_resume_checked) {
		g_boot_resume_checked = true;
		abort_unsafe_resume();
		if (was_active && g_nvs.active == 0U) {
			return true;
		}
	}

	if (g_nvs.active == 0U) {
		g_low_v_streak = 0U;
		g_dc_streak = 0U;
		return false;
	}

	const struct battery_state *bat = battery_monitor_state();

	if (bat == NULL || !bat->valid) {
		return false;
	}

	if (bat->on_dc) {
		g_dc_streak++;
		g_low_v_streak = 0U;
		if (g_dc_streak >= BENCH_DC_STREAK_STOP) {
			disarm_bench_auto("USB/DC detected");
		}
		return was_active && g_nvs.active == 0U;
	}

	g_dc_streak = 0U;

	if (bat->voltage_v < BATTERY_BENCH_STOP_V) {
		g_low_v_streak++;
		if (g_low_v_streak >= BENCH_LOW_STREAK_STOP) {
			disarm_bench_auto("cell below stop threshold");
		}
	} else {
		g_low_v_streak = 0U;
	}

	return was_active && g_nvs.active == 0U;
}

void battery_bench_snapshot_fill(struct battery_bench_snapshot *out)
{
	if (out == NULL) {
		return;
	}
	out->session_id = g_nvs.session_id;
	out->sample_seq = g_nvs.sample_seq;
	out->uptime_ms = k_uptime_get_32();
	out->cpu_mhz = power_manager_cpu_mhz_settled();
	out->imu_hz = power_manager_imu_hz_target();
	out->render_hz = power_manager_render_hz_target();
	out->screen_on = power_manager_screen_on() ? 1U : 0U;
}

SETTINGS_STATIC_HANDLER_DEFINE(bat_bench, "bench", NULL, bench_settings_set, NULL, NULL);
