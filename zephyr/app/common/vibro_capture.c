#include "vibro_capture.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "device_config.h"
#include "vibro_features.h"
#include "vibro_band_rms.h"
#include "vibro_ref_store.h"
#include "vibro_schedule.h"
#include "clock_sync.h"
#include "vibro_verdict_store.h"

LOG_MODULE_REGISTER(vibro_cap, LOG_LEVEL_INF);

struct vibro_capture_state {
	struct imu_sample ring[VIBRO_CAPTURE_MAX_SAMPLES];
	size_t ring_len;
	size_t ring_head;

	/* Live/active reference — either mid-recording or loaded from a flash
	 * slot (vibro_ref_store). Magnitude-only: verdicts never need the raw
	 * 7-axis samples, only the scalar magnitude series (see pearson_corr /
	 * band_vector_corr below), so this is ~14x more compact per sample
	 * than the old struct imu_sample ref[] and lets a slot hold a much
	 * longer (closer to the 30s vision target) recording for the same RAM. */
	float ref_mag[VIBRO_REF_MAG_MAX];
	size_t ref_len;
	float ref_rms;
	float ref_peak;
	bool ref_recording;
	uint8_t ref_recording_slot;
	char ref_recording_name[VIBRO_REF_NAME_MAX];
	uint32_t ref_record_start_ms;
	uint32_t ref_record_start_unix;
	size_t ref_max_record_samples;
	double ref_rms_accum;

	uint32_t last_verdict_seq;
	uint32_t last_ack_seq;
	uint16_t pending_count;
	uint32_t decim_counter;
	uint8_t decim_factor;
	bool window_active;
	uint32_t last_persist_seq;
	uint32_t last_persist_ms;
	struct vibro_band_rms bands;
	struct vibro_band_rms ref_bands;
};

static struct vibro_capture_state g_vib;

static float magnitude_g(const struct imu_sample *s)
{
	return sqrtf(s->ax * s->ax + s->ay * s->ay + s->az * s->az);
}

static float pearson_corr(const float *a, const float *b, size_t n)
{
	double ma = 0.0;
	double mb = 0.0;

	if (n < 8) {
		return 1.0f;
	}

	for (size_t i = 0; i < n; i++) {
		ma += a[i];
		mb += b[i];
	}
	ma /= (double)n;
	mb /= (double)n;

	double num = 0.0;
	double da = 0.0;
	double db = 0.0;

	for (size_t i = 0; i < n; i++) {
		const double xa = a[i] - ma;
		const double xb = b[i] - mb;

		num += xa * xb;
		da += xa * xa;
		db += xb * xb;
	}

	if (da < 1e-12 || db < 1e-12) {
		return 1.0f;
	}

	float r = (float)(num / sqrt(da * db));

	if (r < -1.0f) {
		return -1.0f;
	}
	if (r > 1.0f) {
		return 1.0f;
	}
	return r;
}

static enum vibro_level level_max(enum vibro_level a, enum vibro_level b)
{
	return (enum vibro_level)((a > b) ? a : b);
}

static enum vibro_level level_from_metrics(float rms_delta, float corr, bool has_ref)
{
	if (!has_ref) {
		return VIBRO_LEVEL_OK;
	}
	if (rms_delta >= VIBRO_ALERT_RMS_DELTA_G || corr <= VIBRO_ALERT_CORR) {
		return VIBRO_LEVEL_ALERT;
	}
	if (rms_delta >= VIBRO_WARN_RMS_DELTA_G || corr <= VIBRO_WARN_CORR) {
		return VIBRO_LEVEL_WARN;
	}
	return VIBRO_LEVEL_OK;
}

static float band_vector_corr(const struct vibro_band_rms *a, const struct vibro_band_rms *b)
{
	double dot = 0.0;
	double na = 0.0;
	double nb = 0.0;

	if (a == NULL || b == NULL || !a->valid || !b->valid) {
		return 1.0f;
	}

	for (size_t i = 0; i < VIBRO_BAND_COUNT; i++) {
		dot += (double)a->bands[i] * (double)b->bands[i];
		na += (double)a->bands[i] * (double)a->bands[i];
		nb += (double)b->bands[i] * (double)b->bands[i];
	}

	if (na < 1e-12 || nb < 1e-12) {
		return 1.0f;
	}

	return (float)(dot / sqrt(na * nb));
}

static float band_delta_max(const struct vibro_band_rms *live, const struct vibro_band_rms *ref)
{
	float max_d = 0.0f;

	if (live == NULL || ref == NULL || !live->valid || !ref->valid) {
		return 0.0f;
	}

	for (size_t i = 0; i < VIBRO_BAND_COUNT; i++) {
		const float denom = ref->bands[i] > 1e-4f ? ref->bands[i] : 1e-4f;
		const float d = fabsf(live->bands[i] - ref->bands[i]) / denom;

		if (d > max_d) {
			max_d = d;
		}
	}
	return max_d;
}

static enum vibro_level level_from_bands(float band_corr, float band_delta, bool has_band_ref)
{
	if (!has_band_ref) {
		return VIBRO_LEVEL_OK;
	}
	if (band_corr <= VIBRO_ALERT_BAND_CORR || band_delta >= VIBRO_ALERT_BAND_DELTA) {
		return VIBRO_LEVEL_ALERT;
	}
	if (band_corr <= VIBRO_WARN_BAND_CORR || band_delta >= VIBRO_WARN_BAND_DELTA) {
		return VIBRO_LEVEL_WARN;
	}
	return VIBRO_LEVEL_OK;
}

static void rotate_after_ack(void)
{
	const size_t k_keep = 64;

	if (g_vib.ring_len <= k_keep) {
		return;
	}

	/*
	 * VIBRO_CAPTURE_MAX_SAMPLES * sizeof(imu_sample) = 256*28 = 7168 bytes — far
	 * too large for a stack local on any of this app's worker threads/workqueues
	 * (several are 2-8KB total). This function is only ever driven by the single
	 * sequential g_vib state machine (no re-entrancy), so `static` is safe and
	 * moves the buffer to BSS instead of risking a stack overflow (which on
	 * Xtensa/ESP32 manifests as a double-exception -> hardware watchdog reset
	 * that bypasses crash_report entirely).
	 */
	static struct imu_sample compact[VIBRO_CAPTURE_MAX_SAMPLES];
	const size_t n = g_vib.ring_len;
	size_t copied = 0;

	for (size_t i = n - k_keep; i < n; i++) {
		const size_t idx = (n < VIBRO_CAPTURE_MAX_SAMPLES) ? i :
								   (g_vib.ring_head + i) %
									   VIBRO_CAPTURE_MAX_SAMPLES;

		compact[copied++] = g_vib.ring[idx];
	}

	for (size_t i = 0; i < copied; i++) {
		g_vib.ring[i] = compact[i];
	}
	g_vib.ring_len = copied;
	g_vib.ring_head = copied % VIBRO_CAPTURE_MAX_SAMPLES;
}

void vibro_capture_init(void)
{
	memset(&g_vib, 0, sizeof(g_vib));
	g_vib.decim_factor = 1;
	(void)vibro_verdict_store_init();
	(void)vibro_ref_store_init();

	const uint32_t flash_seq = vibro_verdict_store_last_seq();
	const uint16_t flash_pending = vibro_verdict_store_pending_count();

	if (flash_seq > g_vib.last_verdict_seq) {
		g_vib.last_verdict_seq = flash_seq;
	}
	if (flash_pending > g_vib.pending_count) {
		g_vib.pending_count = flash_pending;
	}

	const int8_t active = vibro_ref_store_active_slot();

	if (active >= 0) {
		const int err = vibro_capture_select_reference((uint8_t)active);

		LOG_INF("vibro ref auto-restore slot=%d -> %s", active,
			err == 0 ? "ok" : "failed");
	}
}

void vibro_capture_apply_config(const struct device_config_v1 *cfg)
{
	if (cfg == NULL) {
		return;
	}

	if (cfg->vibro_capture_tier >= 3) {
		g_vib.decim_factor = 8;
	} else if (cfg->vibro_capture_tier >= 2) {
		g_vib.decim_factor = 4;
	} else if (cfg->vibro_capture_tier >= 1) {
		g_vib.decim_factor = 2;
	} else {
		g_vib.decim_factor = 1;
	}
	g_vib.decim_counter = 0;
}

void vibro_capture_reset(void)
{
	g_vib.ring_len = 0;
	g_vib.ring_head = 0;
}

void vibro_capture_push(const struct imu_sample *sample)
{
	const struct device_config_v1 *cfg;

	if (sample == NULL) {
		return;
	}

	cfg = device_config_runtime();
	if (!g_vib.ref_recording &&
	    !vibro_schedule_capture_active(cfg, clock_sync_now_ms32())) {
		return;
	}

	if (g_vib.decim_factor > 1) {
		g_vib.decim_counter = (g_vib.decim_counter + 1) % g_vib.decim_factor;
		if (g_vib.decim_counter != 0) {
			return;
		}
	}

	g_vib.ring[g_vib.ring_head] = *sample;
	g_vib.ring_head = (g_vib.ring_head + 1) % VIBRO_CAPTURE_MAX_SAMPLES;
	if (g_vib.ring_len < VIBRO_CAPTURE_MAX_SAMPLES) {
		g_vib.ring_len++;
	}

	if (g_vib.ref_recording && g_vib.ref_len < g_vib.ref_max_record_samples &&
	    g_vib.ref_len < VIBRO_REF_MAG_MAX) {
		const float mag = magnitude_g(sample);

		g_vib.ref_mag[g_vib.ref_len++] = mag;
		g_vib.ref_rms_accum += (double)mag * (double)mag;
		if (mag > g_vib.ref_peak) {
			g_vib.ref_peak = mag;
		}
		if (g_vib.ref_len >= g_vib.ref_max_record_samples ||
		    g_vib.ref_len >= VIBRO_REF_MAG_MAX) {
			(void)vibro_capture_stop_reference();
		}
	}
}

static float ref_mag_at(void *ctx, size_t index)
{
	struct vibro_capture_state *v = ctx;

	return v->ref_mag[index];
}

static float sample_hz_for_capture(void)
{
	const struct device_config_v1 *cfg = device_config_runtime();
	float hz = (cfg != NULL && cfg->imu_sample_hz > 0U) ? (float)cfg->imu_sample_hz : 10.0f;

	if (g_vib.decim_factor > 1U) {
		hz /= (float)g_vib.decim_factor;
	}
	return hz;
}

bool vibro_capture_start_reference(uint8_t slot, const char *name)
{
	const float hz = sample_hz_for_capture();
	const uint32_t by_time =
		(hz > 0.5f) ? (uint32_t)(VIBRO_REF_MAX_RECORD_SEC * hz) : VIBRO_REF_MAG_MAX;

	if (slot >= VIBRO_REF_STORE_SLOTS) {
		return false;
	}

	g_vib.ref_len = 0;
	g_vib.ref_recording = true;
	g_vib.ref_recording_slot = slot;
	g_vib.ref_max_record_samples = MIN(by_time, (uint32_t)VIBRO_REF_MAG_MAX);
	g_vib.ref_record_start_ms = k_uptime_get_32();
	g_vib.ref_record_start_unix =
		clock_sync_is_synced() ? clock_sync_now_unix_sec() : 0U;
	g_vib.ref_rms_accum = 0.0;
	g_vib.ref_peak = 0.0f;
	memset(&g_vib.ref_bands, 0, sizeof(g_vib.ref_bands));
	if (name != NULL && name[0] != '\0') {
		snprintf(g_vib.ref_recording_name, sizeof(g_vib.ref_recording_name), "%s", name);
	} else {
		snprintf(g_vib.ref_recording_name, sizeof(g_vib.ref_recording_name), "slot %u",
			 slot);
	}
	LOG_INF("vibro reference recording started slot=%u max_samples=%u", slot,
		(unsigned)g_vib.ref_max_record_samples);
	return true;
}

bool vibro_capture_stop_reference(void)
{
	struct vibro_ref_profile prof;

	if (!g_vib.ref_recording) {
		return g_vib.ref_len > 0;
	}

	g_vib.ref_recording = false;
	if (g_vib.ref_len < 16U) {
		LOG_WRN("vibro reference stop: too short (len=%u), discarding",
			(unsigned)g_vib.ref_len);
		g_vib.ref_len = 0;
		return false;
	}

	g_vib.ref_rms = sqrtf((float)(g_vib.ref_rms_accum / (double)g_vib.ref_len));
	g_vib.ref_bands = vibro_band_rms_compute_series(g_vib.ref_len, sample_hz_for_capture(),
							ref_mag_at, &g_vib);

	memset(&prof, 0, sizeof(prof));
	snprintf(prof.name, sizeof(prof.name), "%s", g_vib.ref_recording_name);
	prof.created_unix = g_vib.ref_record_start_unix;
	prof.updated_unix = prof.created_unix;
	prof.duration_ms = k_uptime_get_32() - g_vib.ref_record_start_ms;
	prof.sample_hz = sample_hz_for_capture();
	prof.mag_len = g_vib.ref_len;
	prof.rms = g_vib.ref_rms;
	prof.peak = g_vib.ref_peak;
	prof.band_valid = g_vib.ref_bands.valid;
	memcpy(prof.band_rms, g_vib.ref_bands.bands, sizeof(prof.band_rms));
	memcpy(prof.mag, g_vib.ref_mag, g_vib.ref_len * sizeof(float));

	if (vibro_ref_store_write(g_vib.ref_recording_slot, &prof) != 0) {
		LOG_ERR("vibro reference stop: flash write failed slot=%u",
			g_vib.ref_recording_slot);
		return false;
	}
	(void)vibro_ref_store_set_active(g_vib.ref_recording_slot);
	LOG_INF("vibro reference stopped slot=%u len=%u dur=%ums", g_vib.ref_recording_slot,
		(unsigned)g_vib.ref_len, prof.duration_ms);
	return true;
}

bool vibro_capture_reference_ready(void)
{
	return g_vib.ref_len > 0;
}

size_t vibro_capture_reference_len(void)
{
	return g_vib.ref_len;
}

int vibro_capture_select_reference(uint8_t slot)
{
	static struct vibro_ref_profile prof; /* ~2KB — off the stack, see vibro_ref_store.c */
	int err = vibro_ref_store_read(slot, &prof);

	if (err != 0) {
		return err;
	}

	g_vib.ref_recording = false;
	g_vib.ref_len = MIN(prof.mag_len, (uint32_t)VIBRO_REF_MAG_MAX);
	memcpy(g_vib.ref_mag, prof.mag, g_vib.ref_len * sizeof(float));
	g_vib.ref_rms = prof.rms;
	g_vib.ref_peak = prof.peak;
	g_vib.ref_bands.valid = prof.band_valid;
	memcpy(g_vib.ref_bands.bands, prof.band_rms, sizeof(g_vib.ref_bands.bands));

	return vibro_ref_store_set_active((int8_t)slot);
}

int vibro_capture_delete_reference(uint8_t slot)
{
	const bool was_active = vibro_ref_store_active_slot() == (int8_t)slot;
	int err = vibro_ref_store_delete(slot);

	if (err == 0 && was_active) {
		g_vib.ref_len = 0;
		g_vib.ref_rms = 0.0f;
		g_vib.ref_peak = 0.0f;
		memset(&g_vib.ref_bands, 0, sizeof(g_vib.ref_bands));
	}
	return err;
}

int8_t vibro_capture_active_reference_slot(void)
{
	return vibro_ref_store_active_slot();
}

int vibro_capture_list_references_json(char *buf, size_t len)
{
	return vibro_ref_store_list_json(buf, len);
}

struct vibro_metrics vibro_capture_metrics_live(void)
{
	struct vibro_metrics out = { 0 };

	if (g_vib.ring_len == 0) {
		return out;
	}

	double sum_sq = 0.0;
	float peak = 0.0f;

	if (g_vib.ring_len < VIBRO_CAPTURE_MAX_SAMPLES) {
		for (size_t i = 0; i < g_vib.ring_len; i++) {
			const float mag = magnitude_g(&g_vib.ring[i]);

			sum_sq += (double)mag * (double)mag;
			if (mag > peak) {
				peak = mag;
			}
		}
	} else {
		for (size_t i = 0; i < VIBRO_CAPTURE_MAX_SAMPLES; i++) {
			const size_t idx = (g_vib.ring_head + i) % VIBRO_CAPTURE_MAX_SAMPLES;
			const float mag = magnitude_g(&g_vib.ring[idx]);

			sum_sq += (double)mag * (double)mag;
			if (mag > peak) {
				peak = mag;
			}
		}
	}

	out.rms_g = (float)sqrt(sum_sq / (double)g_vib.ring_len);
	out.peak_g = peak;
	out.samples = (uint32_t)g_vib.ring_len;
	out.valid = true;
	return out;
}

struct ring_mag_ctx {
	const struct vibro_capture_state *v;
};

static float ring_mag_at(void *ctx, size_t index)
{
	const struct ring_mag_ctx *c = ctx;
	const size_t idx = c->v->ring_len < VIBRO_CAPTURE_MAX_SAMPLES ?
				   index :
				   (c->v->ring_head + index) % VIBRO_CAPTURE_MAX_SAMPLES;

	return sqrtf(c->v->ring[idx].ax * c->v->ring[idx].ax + c->v->ring[idx].ay * c->v->ring[idx].ay +
		     c->v->ring[idx].az * c->v->ring[idx].az);
}

struct vibro_edge_features vibro_capture_edge_features(void)
{
	struct ring_mag_ctx ctx;
	size_t n;

	if (g_vib.ring_len == 0U) {
		return (struct vibro_edge_features){ 0 };
	}

	n = g_vib.ring_len;
	if (n > VIBRO_CAPTURE_MAX_SAMPLES) {
		n = VIBRO_CAPTURE_MAX_SAMPLES;
	}

	ctx.v = &g_vib;

	const struct device_config_v1 *cfg = device_config_runtime();
	float hz = (cfg != NULL && cfg->imu_sample_hz > 0U) ? (float)cfg->imu_sample_hz : 10.0f;

	if (g_vib.decim_factor > 1U) {
		hz /= (float)g_vib.decim_factor;
	}

	return vibro_features_compute_series(n, hz, ring_mag_at, &ctx);
}

static struct vibro_band_rms compute_bands_from_ring(void)
{
	struct ring_mag_ctx ctx;
	size_t n;

	if (g_vib.ring_len == 0U) {
		return (struct vibro_band_rms){ 0 };
	}

	n = g_vib.ring_len;
	if (n > VIBRO_CAPTURE_MAX_SAMPLES) {
		n = VIBRO_CAPTURE_MAX_SAMPLES;
	}

	ctx.v = &g_vib;

	const struct device_config_v1 *cfg = device_config_runtime();
	float hz = (cfg != NULL && cfg->imu_sample_hz > 0U) ? (float)cfg->imu_sample_hz : 10.0f;

	if (g_vib.decim_factor > 1U) {
		hz /= (float)g_vib.decim_factor;
	}

	return vibro_band_rms_compute_series(n, hz, ring_mag_at, &ctx);
}

struct vibro_band_rms vibro_capture_band_rms(void)
{
	if (!g_vib.bands.valid && g_vib.ring_len >= 16U) {
		g_vib.bands = compute_bands_from_ring();
	}
	return g_vib.bands;
}

uint32_t vibro_capture_pending_session_seq(void)
{
	return vibro_verdict_store_first_pending_seq();
}

struct vibro_verdict vibro_capture_verdict(void)
{
	struct vibro_verdict out = { 0 };
	const struct vibro_metrics live = vibro_capture_metrics_live();

	if (!live.valid) {
		return out;
	}

	out.rms_g = live.rms_g;
	out.peak_g = live.peak_g;
	out.valid = true;
	out.has_reference = g_vib.ref_len > 0;
	out.corr = 1.0f;
	if (!out.has_reference) {
		out.level = VIBRO_LEVEL_OK;
		return out;
	}

	out.rms_delta = fabsf(live.rms_g - g_vib.ref_rms);
	out.peak_delta = fabsf(live.peak_g - g_vib.ref_peak);

	const size_t n = g_vib.ring_len < g_vib.ref_len ? g_vib.ring_len : g_vib.ref_len;
	/* 256 floats = 1KB — moved off the stack for the same reason as the
	 * `compact` buffer in rotate_after_ack() above (single-sequential caller).
	 * g_vib.ref_mag is already part of the static g_vib state, no stack risk. */
	static float live_mag[VIBRO_CAPTURE_MAX_SAMPLES];

	for (size_t i = 0; i < n; i++) {
		const size_t ring_idx = g_vib.ring_len < VIBRO_CAPTURE_MAX_SAMPLES ?
					      i :
					      (g_vib.ring_head + (g_vib.ring_len - n + i)) %
						      VIBRO_CAPTURE_MAX_SAMPLES;

		live_mag[i] = magnitude_g(&g_vib.ring[ring_idx]);
	}

	out.corr = pearson_corr(live_mag, g_vib.ref_mag, n);
	out.has_band_ref = g_vib.ref_bands.valid;
	if (out.has_band_ref) {
		const struct vibro_band_rms live_bands = vibro_capture_band_rms();

		if (live_bands.valid) {
			out.band_corr = band_vector_corr(&live_bands, &g_vib.ref_bands);
			out.band_delta_max = band_delta_max(&live_bands, &g_vib.ref_bands);
		} else {
			out.band_corr = 1.0f;
			out.band_delta_max = 0.0f;
		}
	}

	out.level = level_max(level_from_metrics(out.rms_delta, out.corr, true),
			      level_from_bands(out.band_corr, out.band_delta_max, out.has_band_ref));
	return out;
}

static void persist_verdict(uint32_t seq)
{
	const struct vibro_verdict verdict = vibro_capture_verdict();
	const struct vibro_edge_features edge = vibro_capture_edge_features();
	const struct vibro_band_rms bands = vibro_capture_band_rms();

	if (!verdict.valid) {
		return;
	}

	(void)vibro_verdict_store_append(seq, k_uptime_get_32(), &verdict, &edge, &bands);
}

static void session_end_commit(void)
{
	uint32_t seq;

	if (g_vib.ring_len < 16U) {
		return;
	}

	g_vib.bands = compute_bands_from_ring();

	seq = vibro_verdict_store_alloc_seq();
	if (seq == 0U) {
		return;
	}

	g_vib.last_verdict_seq = seq;
	persist_verdict(seq);
	g_vib.pending_count = vibro_verdict_store_pending_count();
	if (g_vib.pending_count == 0U) {
		g_vib.pending_count = 1U;
	}
	LOG_INF("capture session commit seq=%u bands=%u", seq, g_vib.bands.valid ? 1U : 0U);
}

void vibro_capture_session_tick(uint32_t now_ms)
{
	const struct device_config_v1 *cfg = device_config_runtime();
	const bool active = vibro_schedule_capture_active(cfg, now_ms);

	if (g_vib.window_active && !active && cfg != NULL &&
	    cfg->vibro_schedule_mode != VIBRO_SCHEDULE_ALWAYS) {
		session_end_commit();
	} else if (active && !g_vib.window_active) {
		memset(&g_vib.bands, 0, sizeof(g_vib.bands));
	}

	g_vib.window_active = active;
	ARG_UNUSED(now_ms);
}

/* Live STATUS polls can run at IMU rate (~30 Hz). Flash spool append + wrap-erase must not
 * run every batch — that stacks against the grace-elapse traffic burst and trips TG0WDT. */
#define VERDICT_FLASH_PERSIST_MIN_MS 30000U

void vibro_capture_on_status_seq(uint32_t seq, bool persist_flash)
{
	g_vib.last_verdict_seq = seq;
	if (persist_flash && seq != g_vib.last_persist_seq) {
		const uint32_t now = k_uptime_get_32();

		if (g_vib.last_persist_ms == 0U ||
		    (now - g_vib.last_persist_ms) >= VERDICT_FLASH_PERSIST_MIN_MS) {
			g_vib.last_persist_ms = now;
			g_vib.last_persist_seq = seq;
			persist_verdict(seq);
		}
	}
	if (seq > g_vib.last_ack_seq) {
		g_vib.pending_count = vibro_verdict_store_pending_count();
		if (g_vib.pending_count == 0U && persist_flash) {
			g_vib.pending_count = 1U;
		}
	} else {
		g_vib.pending_count = vibro_verdict_store_pending_count();
	}
}

bool vibro_capture_ack_offload(uint32_t seq)
{
	if (seq <= g_vib.last_ack_seq) {
		return true;
	}
	if (seq < g_vib.last_verdict_seq && g_vib.last_verdict_seq != 0) {
		return false;
	}
	g_vib.last_ack_seq = seq;
	g_vib.pending_count = vibro_verdict_store_pending_count();
	(void)vibro_verdict_store_ack(seq);
	rotate_after_ack();
	return true;
}

uint32_t vibro_capture_last_ack_seq(void)
{
	return g_vib.last_ack_seq;
}

uint16_t vibro_capture_pending_offload_count(void)
{
	const uint16_t flash_pending = vibro_verdict_store_pending_count();

	if (flash_pending > g_vib.pending_count) {
		return flash_pending;
	}
	return g_vib.pending_count;
}
