#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include "clock_sync.h"
#include "flash_safety.h"
#include "floor_calib.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

LOG_MODULE_REGISTER(floor_cal, LOG_LEVEL_INF);

#define FLOOR_CALIB_MAGIC 0x464C4331U /* "FLC1" */

struct floor_calib_persist {
	uint32_t magic;
	struct mat3 r;
	uint8_t valid;
	uint8_t _pad[3];
	float residual_deg;
	uint32_t created_unix;
	uint32_t crc32;
} __packed;

static struct floor_calib_persist g_st;
static bool g_loaded;
static atomic_t g_dirty;

/* Sampling window state — not persisted. */
static bool g_sampling;
static int64_t g_sample_start_ms;
static uint16_t g_sample_duration_ms;
static double g_acc_x, g_acc_y, g_acc_z;
static uint32_t g_acc_n;

static uint32_t persist_crc(const struct floor_calib_persist *p)
{
	return crc32_ieee((const uint8_t *)p, offsetof(struct floor_calib_persist, crc32));
}

static bool persist_valid(const struct floor_calib_persist *p)
{
	return p->magic == FLOOR_CALIB_MAGIC && p->crc32 == persist_crc(p);
}

static int settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *leaf = name;

	if (settings_name_steq(name, "state", &leaf) && leaf == NULL) {
		if (len != sizeof(g_st)) {
			return -EINVAL;
		}
		if (read_cb(cb_arg, &g_st, sizeof(g_st)) != (ssize_t)sizeof(g_st)) {
			return -EIO;
		}
		g_loaded = true;
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(floor_cal, "floor_cal", NULL, settings_set, NULL, NULL);

static void persist_defaults(struct floor_calib_persist *p)
{
	memset(p, 0, sizeof(*p));
	p->r = mat3_identity();
	p->valid = 0U;
	p->residual_deg = 0.0f;
}

void floor_calib_init(void)
{
	persist_defaults(&g_st);
	(void)settings_load_subtree("floor_cal");
	if (!g_loaded || !persist_valid(&g_st)) {
		persist_defaults(&g_st);
		LOG_INF("floor calib: none stored (identity)");
	} else {
		LOG_INF("floor calib: loaded (residual was %.2f deg)", (double)g_st.residual_deg);
	}
}

static int persist_save(void)
{
	g_st.magic = FLOOR_CALIB_MAGIC;
	g_st.crc32 = persist_crc(&g_st);
	return settings_save_one("floor_cal/state", &g_st, sizeof(g_st));
}

void floor_calib_poll(void)
{
	if (!atomic_get(&g_dirty)) {
		return;
	}
	/* Almost every real invocation of this feature happens over an active BLE connection
	 * (the user is holding the phone, watching progress) — same flash_area_erase()-during-
	 * live-BLE hazard as every other flash-backed store (see flash_safety.h). Defer. */
	if (!app_flash_erase_safe()) {
		return;
	}
	if (!atomic_cas(&g_dirty, 1, 0)) {
		return;
	}
	if (persist_save() != 0) {
		LOG_ERR("floor calib: settings_save_one failed");
	}
}

void floor_calib_start(uint16_t duration_ms)
{
	if (duration_ms == 0U) {
		duration_ms = FLOOR_CALIB_DEFAULT_DURATION_MS;
	}
	duration_ms = (uint16_t)CLAMP(duration_ms, FLOOR_CALIB_MIN_DURATION_MS,
				      FLOOR_CALIB_MAX_DURATION_MS);

	g_acc_x = 0.0;
	g_acc_y = 0.0;
	g_acc_z = 0.0;
	g_acc_n = 0U;
	g_sample_duration_ms = duration_ms;
	g_sample_start_ms = k_uptime_get();
	g_sampling = true;
	LOG_INF("floor calib: sampling started (%u ms)", duration_ms);
}

void floor_calib_clear(void)
{
	g_sampling = false;
	persist_defaults(&g_st);
	atomic_set(&g_dirty, 1);
	LOG_INF("floor calib: cleared");
}

/* Rodrigues' rotation-between-two-unit-vectors: returns R such that R * a == b. */
static struct mat3 mat3_from_vectors(struct vec3 a, struct vec3 b)
{
	const struct vec3 v = vec3_make(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
					 a.x * b.y - a.y * b.x);
	const float c = vec3_dot(a, b);
	const float s2 = vec3_dot(v, v);

	if (s2 < 1e-12f) {
		if (c > 0.0f) {
			return mat3_identity(); /* already aligned */
		}
		/* Antiparallel (180°): pick any axis orthogonal to a. */
		struct vec3 axis = (fabsf(a.x) < 0.9f) ? vec3_make(1.0f, 0.0f, 0.0f)
							: vec3_make(0.0f, 1.0f, 0.0f);
		struct vec3 ortho =
			vec3_norm(vec3_make(a.y * axis.z - a.z * axis.y, a.z * axis.x - a.x * axis.z,
					     a.x * axis.y - a.y * axis.x));
		struct mat3 r = mat3_identity();

		/* 180° rotation about `ortho`: R = 2*ortho*ortho^T - I */
		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) {
				const float o_i = (i == 0) ? ortho.x : (i == 1) ? ortho.y : ortho.z;
				const float o_j = (j == 0) ? ortho.x : (j == 1) ? ortho.y : ortho.z;

				r.m[i][j] = 2.0f * o_i * o_j - ((i == j) ? 1.0f : 0.0f);
			}
		}
		return r;
	}

	struct mat3 vx = { .m = { { 0.0f, -v.z, v.y }, { v.z, 0.0f, -v.x }, { -v.y, v.x, 0.0f } } };
	struct mat3 vx2 = mat3_mul(vx, vx);
	const float k = (1.0f - c) / s2;
	struct mat3 r = mat3_identity();

	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			r.m[i][j] += vx.m[i][j] + vx2.m[i][j] * k;
		}
	}
	return r;
}

static void finish_sampling(void)
{
	g_sampling = false;

	if (g_acc_n == 0U) {
		LOG_WRN("floor calib: no samples collected, aborting");
		return;
	}

	struct vec3 avg =
		vec3_make((float)(g_acc_x / (double)g_acc_n), (float)(g_acc_y / (double)g_acc_n),
			  (float)(g_acc_z / (double)g_acc_n));
	const float mag = vec3_len(avg);

	if (mag < 0.5f || mag > 1.5f) {
		LOG_WRN("floor calib: implausible gravity magnitude %.3f g, aborting",
			(double)mag);
		return;
	}

	struct vec3 avg_norm = vec3_norm(avg);
	struct vec3 target = vec3_make(0.0f, 0.0f, 1.0f); /* az == +1g at rest, see attitude.c */
	const float residual_deg =
		(float)(acosf(CLAMP(vec3_dot(avg_norm, target), -1.0f, 1.0f)) * (180.0f / (float)M_PI));

	g_st.r = mat3_from_vectors(avg_norm, target);
	g_st.valid = 1U;
	g_st.residual_deg = residual_deg;
	g_st.created_unix = clock_sync_is_synced() ? clock_sync_now_unix_sec() : 0U;
	atomic_set(&g_dirty, 1);

	LOG_INF("floor calib: done, corrected %.2f deg of mounting tilt (n=%u samples)",
		(double)residual_deg, g_acc_n);
}

void floor_calib_feed(const struct imu_sample *sample)
{
	if (!g_sampling || sample == NULL) {
		return;
	}

	g_acc_x += (double)sample->ax;
	g_acc_y += (double)sample->ay;
	g_acc_z += (double)sample->az;
	g_acc_n++;

	if ((k_uptime_get() - g_sample_start_ms) >= (int64_t)g_sample_duration_ms) {
		finish_sampling();
	}
}

void floor_calib_apply(struct imu_sample *sample)
{
	if (!g_st.valid || sample == NULL) {
		return;
	}

	const struct vec3 a = mat3_transform(&g_st.r, vec3_make(sample->ax, sample->ay, sample->az));
	const struct vec3 g = mat3_transform(&g_st.r, vec3_make(sample->gx, sample->gy, sample->gz));

	sample->ax = a.x;
	sample->ay = a.y;
	sample->az = a.z;
	sample->gx = g.x;
	sample->gy = g.y;
	sample->gz = g.z;
}

bool floor_calib_valid(void)
{
	return g_st.valid != 0U;
}

bool floor_calib_sampling(void)
{
	return g_sampling;
}

float floor_calib_progress(void)
{
	if (!g_sampling || g_sample_duration_ms == 0U) {
		return 0.0f;
	}
	const float elapsed = (float)(k_uptime_get() - g_sample_start_ms);

	return CLAMP(elapsed / (float)g_sample_duration_ms, 0.0f, 1.0f);
}

float floor_calib_residual_deg(void)
{
	return g_st.residual_deg;
}

int floor_calib_status_json(char *buf, size_t buf_len)
{
	if (buf == NULL || buf_len == 0U) {
		return 0;
	}

	const int n = snprintf(buf, buf_len,
				"{\"valid\":%u,\"sampling\":%u,\"progress\":%.2f,"
				"\"residual_deg\":%.2f,\"unix\":%u}",
				(unsigned)g_st.valid, (unsigned)g_sampling,
				(double)floor_calib_progress(), (double)g_st.residual_deg,
				(unsigned)g_st.created_unix);

	return (n > 0 && (size_t)n < buf_len) ? n : 0;
}
