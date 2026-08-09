#include "vibro_features.h"

#include <math.h>

static float magnitude_g(const struct imu_sample *s)
{
	return sqrtf(s->ax * s->ax + s->ay * s->ay + s->az * s->az);
}

struct sample_mag_ctx {
	const struct imu_sample *samples;
};

static float sample_mag_at(void *ctx, size_t index)
{
	const struct sample_mag_ctx *c = ctx;

	return magnitude_g(&c->samples[index]);
}

static struct vibro_edge_features compute_from_mags(size_t count, float sample_hz,
						    vibro_mag_at_fn mag_at, void *ctx)
{
	struct vibro_edge_features out = { 0 };

	if (mag_at == NULL || count < 8U || sample_hz <= 0.0f) {
		return out;
	}

	double sum_sq = 0.0;
	float peak = 0.0f;
	float prev_mag = mag_at(ctx, 0);
	float prev_dm = 0.0f;
	uint32_t crossings = 0U;
	bool have_dm = false;

	for (size_t i = 0; i < count; i++) {
		const float mag = mag_at(ctx, i);

		sum_sq += (double)mag * (double)mag;
		if (mag > peak) {
			peak = mag;
		}
		if (i > 0U) {
			const float dm = mag - prev_mag;

			if (have_dm &&
			    ((prev_dm >= 0.0f && dm < 0.0f) || (prev_dm < 0.0f && dm >= 0.0f))) {
				crossings++;
			}
			prev_dm = dm;
			have_dm = true;
		}
		prev_mag = mag;
	}

	const float rms = (float)sqrt(sum_sq / (double)count);

	if (rms < 1e-4f) {
		return out;
	}

	out.crest = peak / rms;
	out.zcr_hz = (float)crossings * sample_hz / (float)(count - 1U);

	{
		double diff_sq = 0.0;
		double mag_sq = 0.0;

		prev_mag = mag_at(ctx, 0);
		for (size_t i = 1; i < count; i++) {
			const float mag = mag_at(ctx, i);
			const float d = mag - prev_mag;

			diff_sq += (double)d * (double)d;
			mag_sq += (double)mag * (double)mag;
			prev_mag = mag;
		}
		mag_sq += (double)mag_at(ctx, 0) * (double)mag_at(ctx, 0);
		if (mag_sq > 1e-8) {
			out.hf_ratio = (float)(diff_sq / mag_sq);
		}
	}

	out.valid = true;
	return out;
}

struct vibro_edge_features vibro_features_compute(const struct imu_sample *samples, size_t count,
						    float sample_hz)
{
	struct sample_mag_ctx ctx = { .samples = samples };

	if (samples == NULL) {
		return (struct vibro_edge_features){ 0 };
	}

	return compute_from_mags(count, sample_hz, sample_mag_at, &ctx);
}

struct vibro_edge_features vibro_features_compute_series(size_t count, float sample_hz,
							 vibro_mag_at_fn mag_at, void *ctx)
{
	return compute_from_mags(count, sample_hz, mag_at, ctx);
}
