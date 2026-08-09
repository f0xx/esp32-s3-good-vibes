#include "device_config.h"

#include <stddef.h>
#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>

#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(devcfg, LOG_LEVEL_INF);

#define SETTINGS_KEY "devcfg/blob"

#if !FIXED_PARTITION_EXISTS(storage_partition)
#error "storage_partition required for device config NVS"
#endif

#define STORAGE_PARTITION_ID FIXED_PARTITION_ID(storage_partition)

static struct device_config_v1 g_runtime;
static atomic_t g_save_pending;
static bool g_work_ready;

static uint32_t calc_crc(const struct device_config_v1 *cfg);
static void flush_save(void);

void device_config_init(void)
{
	if (g_work_ready) {
		return;
	}

	atomic_set(&g_save_pending, 0);
	g_work_ready = true;
}

void device_config_defaults(struct device_config_v1 *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->magic = DEVICE_CONFIG_MAGIC;
	cfg->version = DEVICE_CONFIG_VERSION;
	cfg->size = sizeof(struct device_config_v1);
	cfg->bat_full_v = BAT_FULL_V;
	cfg->bat_empty_v = BAT_EMPTY_V;
	cfg->bat_offset = BAT_MEASUREMENT_OFFSET;
	cfg->bat_dc_margin_v = BAT_DC_MARGIN_V;
	cfg->bat_trend_rise_v = BAT_TREND_RISE_V;
	cfg->bat_trend_fall_v = BAT_TREND_FALL_V;
	cfg->bat_trend_stable_v = BAT_TREND_STABLE_V;
	cfg->bat_trend_window = BAT_TREND_WINDOW;
	cfg->bat_trend_compare = BAT_TREND_COMPARE;
	cfg->bat_dc_confirm = 6;
	cfg->bat_bat_confirm = 3;
	cfg->bat_voltage_ema = BAT_VOLTAGE_EMA;
	cfg->bat_percent_ema = 0.03f;
	cfg->zoom_default[0] = cfg->zoom_default[1] = cfg->zoom_default[2] = 0.75f;
	cfg->zoom_min = 0.05f;
	cfg->zoom_max = 2.0f;
	cfg->zoom_motion_min = 0.25f;
	cfg->zoom_step = 0.02f;
	cfg->zoom_smooth = 0.18f;
	cfg->zoom_accel_mode = 2;
	cfg->walk_height_m = 1.80f;
	cfg->walk_pocket_m = 1.50f;
	cfg->walk_step_min_m = 0.70f;
	cfg->walk_step_max_m = 0.90f;
	cfg->ble_poll_ms = 33;
	cfg->ble_default_mode = 1;
	cfg->imu_accel_scale = 1.0f;
	cfg->imu_gyro_scale = 1.0f;
	cfg->power_profile = POWER_PROFILE_BALANCED;
	cfg->tft_policy = TFT_POLICY_ALWAYS;
	cfg->wake_interval_sec = 3600;
	cfg->active_window_sec = 90;
	cfg->cpu_mhz = 80;
	cfg->imu_sample_hz = 10;
	cfg->auto_dc_profile = 0;
	cfg->vibro_schedule_mode = 0;
	cfg->vibro_interval_sec = 60;
	cfg->vibro_window_sec = 15;
	cfg->vibro_jitter_sec = 0;
	cfg->vibro_capture_tier = 0;
	cfg->crc32 = calc_crc(cfg);
}

static uint32_t calc_crc(const struct device_config_v1 *cfg)
{
	const size_t off = offsetof(struct device_config_v1, crc32) + sizeof(cfg->crc32);

	return crc32_ieee((const uint8_t *)cfg + off, sizeof(*cfg) - off);
}

static bool validate(const struct device_config_v1 *cfg)
{
	return cfg->magic == DEVICE_CONFIG_MAGIC && cfg->version == DEVICE_CONFIG_VERSION &&
	       cfg->size == sizeof(struct device_config_v1) && cfg->crc32 == calc_crc(cfg);
}

static int settings_handler(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(name, "blob") != 0) {
		return -ENOENT;
	}

	if (len != sizeof(struct device_config_v1)) {
		LOG_WRN("devcfg blob size mismatch (%u != %u) — ignoring NVS copy",
			(unsigned)len, (unsigned)sizeof(struct device_config_v1));
		return -EINVAL;
	}

	if (read_cb == NULL) {
		return -EINVAL;
	}

	struct device_config_v1 tmp;

	if (read_cb(cb_arg, &tmp, len) != (ssize_t)len) {
		return -EIO;
	}
	if (!validate(&tmp)) {
		LOG_WRN("devcfg blob CRC/magic invalid — ignoring NVS copy");
		return -EINVAL;
	}

	g_runtime = tmp;
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(devcfg, "devcfg", NULL, settings_handler, NULL, NULL);

static void flush_save(void)
{
	struct device_config_v1 tmp = g_runtime;

	tmp.magic = DEVICE_CONFIG_MAGIC;
	tmp.version = DEVICE_CONFIG_VERSION;
	tmp.size = sizeof(tmp);
	tmp.crc32 = calc_crc(&tmp);

	const int err = settings_save_one(SETTINGS_KEY, &tmp, sizeof(tmp));

	if (err != 0) {
		LOG_ERR("devcfg NVS save failed (%d)", err);
	} else {
		LOG_INF("devcfg saved NVS profile=%u rev=%u", tmp.power_profile,
			tmp.profile_updated_unix);
	}
}

void device_config_poll(void)
{
	if (!g_work_ready || !atomic_cas(&g_save_pending, 1, 0)) {
		return;
	}

	flush_save();
}

bool device_config_storage_erase(void)
{
	(void)settings_delete("devcfg/blob");
	(void)settings_delete("devcfg");

	const struct flash_area *fa;
	int err = flash_area_open(STORAGE_PARTITION_ID, &fa);

	if (err != 0) {
		LOG_ERR("devcfg storage open failed (%d)", err);
		return false;
	}

	err = flash_area_erase(fa, 0, fa->fa_size);
	const size_t erased = fa->fa_size;
	flash_area_close(fa);

	if (err != 0) {
		LOG_ERR("devcfg storage erase failed (%d)", err);
		return false;
	}

	device_config_defaults(&g_runtime);
	LOG_WRN("devcfg storage partition erased (%u B) — fresh defaults pp=%u",
		(unsigned)erased, g_runtime.power_profile);
	return true;
}

bool device_config_load(struct device_config_v1 *cfg)
{
	device_config_init();
	device_config_defaults(&g_runtime);

	const int err = settings_load_subtree("devcfg");

	if (err != 0 && err != -ENOENT) {
		LOG_WRN("devcfg settings_load_subtree failed (%d) — using defaults", err);
	} else if (validate(&g_runtime)) {
		LOG_INF("devcfg loaded NVS profile=%u rev=%u", g_runtime.power_profile,
			g_runtime.profile_updated_unix);
		*cfg = g_runtime;
		return true;
	}

	LOG_INF("devcfg defaults profile=%u (no valid NVS copy)", g_runtime.power_profile);
	*cfg = g_runtime;
	return false;
}

bool device_config_save(const struct device_config_v1 *cfg)
{
	struct device_config_v1 tmp = *cfg;

	tmp.magic = DEVICE_CONFIG_MAGIC;
	tmp.version = DEVICE_CONFIG_VERSION;
	tmp.size = sizeof(tmp);
	tmp.crc32 = calc_crc(&tmp);
	g_runtime = tmp;

	if (!g_work_ready) {
		return false;
	}

	(void)atomic_set(&g_save_pending, 1);
	return true;
}

bool device_config_save_sync(const struct device_config_v1 *cfg)
{
	struct device_config_v1 tmp = *cfg;

	tmp.magic = DEVICE_CONFIG_MAGIC;
	tmp.version = DEVICE_CONFIG_VERSION;
	tmp.size = sizeof(tmp);
	tmp.crc32 = calc_crc(&tmp);
	g_runtime = tmp;

	const int err = settings_save_one(SETTINGS_KEY, &tmp, sizeof(tmp));

	if (err != 0) {
		LOG_ERR("devcfg sync save failed (%d)", err);
		return false;
	}
	LOG_INF("devcfg sync-saved NVS rev=%u", tmp.profile_updated_unix);
	return true;
}

static uint32_t read_u32_le(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static void write_u32_le(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xFFU);
	p[1] = (uint8_t)((v >> 8) & 0xFFU);
	p[2] = (uint8_t)((v >> 16) & 0xFFU);
	p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static void bump_local_revision(struct device_config_v1 *cfg)
{
	uint32_t rev = read_u32_le(&cfg->reserved[4]);

	rev = (rev & ~DEVICE_CONFIG_LOCAL_REV_FLAG) + 1U;
	rev |= DEVICE_CONFIG_LOCAL_REV_FLAG;
	write_u32_le(&cfg->reserved[4], rev);
}

uint32_t device_config_cloud_revision(const struct device_config_v1 *cfg)
{
	return cfg != NULL ? cfg->profile_updated_unix : 0U;
}

uint32_t device_config_local_revision(const struct device_config_v1 *cfg)
{
	return cfg != NULL ? read_u32_le(&cfg->reserved[4]) : 0U;
}

uint8_t device_config_local_flags(const struct device_config_v1 *cfg)
{
	return cfg != NULL ? cfg->reserved[8] : 0U;
}

bool device_config_user_screen_off(void)
{
	return (device_config_local_flags(device_config_runtime()) & DEVICE_CONFIG_LOCAL_TFT_OFF) != 0U;
}

void device_config_set_user_screen(bool on)
{
	struct device_config_v1 cfg = *device_config_runtime();

	if (on) {
		cfg.reserved[8] &= (uint8_t)~DEVICE_CONFIG_LOCAL_TFT_OFF;
	} else {
		cfg.reserved[8] |= DEVICE_CONFIG_LOCAL_TFT_OFF;
	}
	bump_local_revision(&cfg);
	(void)device_config_save(&cfg);
}

bool device_config_staging_mode(void)
{
	return (device_config_local_flags(device_config_runtime()) &
		DEVICE_CONFIG_LOCAL_STAGING_MODE) != 0U;
}

void device_config_set_staging_mode(bool staging)
{
	struct device_config_v1 cfg = *device_config_runtime();

	if (staging) {
		cfg.reserved[8] |= DEVICE_CONFIG_LOCAL_STAGING_MODE;
	} else {
		cfg.reserved[8] &= (uint8_t)~DEVICE_CONFIG_LOCAL_STAGING_MODE;
	}
	bump_local_revision(&cfg);
	(void)device_config_save(&cfg);
}

enum device_config_apply_result device_config_apply_remote(const struct device_config_v1 *incoming)
{
	if (incoming == NULL || !validate(incoming)) {
		return DEVICE_CONFIG_APPLY_INVALID;
	}

	const struct device_config_v1 *cur = device_config_runtime();
	const uint32_t in_rev = incoming->profile_updated_unix;
	const uint32_t cur_rev = cur->profile_updated_unix;

	if (in_rev < cur_rev && cur_rev > 0U) {
		LOG_WRN("devcfg reject stale cloud rev %u < %u", in_rev, cur_rev);
		return DEVICE_CONFIG_APPLY_STALE;
	}

	struct device_config_v1 merged = *incoming;

	merged.reserved[4] = cur->reserved[4];
	merged.reserved[5] = cur->reserved[5];
	merged.reserved[6] = cur->reserved[6];
	merged.reserved[7] = cur->reserved[7];
	merged.reserved[8] = cur->reserved[8];

	if (!device_config_save(&merged)) {
		return DEVICE_CONFIG_APPLY_INVALID;
	}
	LOG_INF("devcfg applied cloud rev %u (local rev 0x%08x)", in_rev,
		(unsigned)device_config_local_revision(&merged));
	return DEVICE_CONFIG_APPLY_OK;
}

bool device_config_apply_blob(const uint8_t *blob, size_t len)
{
	if (blob == NULL || len != sizeof(struct device_config_v1)) {
		return false;
	}

	struct device_config_v1 tmp;

	memcpy(&tmp, blob, sizeof(tmp));
	return device_config_apply_remote(&tmp) == DEVICE_CONFIG_APPLY_OK;
}

bool device_config_reload(struct device_config_v1 *cfg)
{
	return device_config_load(cfg);
}

const struct device_config_v1 *device_config_runtime(void)
{
	return &g_runtime;
}
