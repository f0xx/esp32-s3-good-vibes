#include "vibro_band_rms.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void fft_in_place(float *re, float *im, size_t n)
{
	size_t j = 0;

	for (size_t i = 1; i < n; i++) {
		size_t bit = n >> 1;

		while (j & bit) {
			j ^= bit;
			bit >>= 1;
		}
		j ^= bit;
		if (i < j) {
			const float tr = re[i];

			re[i] = re[j];
			re[j] = tr;
			const float ti = im[i];

			im[i] = im[j];
			im[j] = ti;
		}
	}

	for (size_t len = 2; len <= n; len <<= 1) {
		const float ang = -2.0f * (float)M_PI / (float)len;
		const float wlen_re = cosf(ang);
		const float wlen_im = sinf(ang);

		for (size_t i = 0; i < n; i += len) {
			float w_re = 1.0f;
			float w_im = 0.0f;

			for (size_t k = 0; k < len / 2; k++) {
				const float u_re = re[i + k];
				const float u_im = im[i + k];
				const float v_re = re[i + k + len / 2] * w_re - im[i + k + len / 2] * w_im;
				const float v_im = re[i + k + len / 2] * w_im + im[i + k + len / 2] * w_re;

				re[i + k] = u_re + v_re;
				im[i + k] = u_im + v_im;
				re[i + k + len / 2] = u_re - v_re;
				im[i + k + len / 2] = u_im - v_im;

				const float next_w_re = w_re * wlen_re - w_im * wlen_im;
				w_im = w_re * wlen_im + w_im * wlen_re;
				w_re = next_w_re;
			}
		}
	}
}

static float hanning(size_t i, size_t n)
{
	return 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(n - 1U)));
}

struct vibro_band_rms vibro_band_rms_compute_series(size_t count, float sample_hz,
						    vibro_mag_at_fn mag_at, void *ctx)
{
	struct vibro_band_rms out = { 0 };
	/* Off the stack: this runs nested several calls deep inside the vibro
	 * verdict/reference path on threads with limited stack budget. */
	static float re[VIBRO_BAND_FFT_SIZE];
	static float im[VIBRO_BAND_FFT_SIZE];
	size_t n = VIBRO_BAND_FFT_SIZE;
	size_t use = count;

	if (mag_at == NULL || count < 16U || sample_hz <= 0.0f) {
		return out;
	}

	if (use > n) {
		use = n;
	}

	memset(re, 0, n * sizeof(re[0]));
	memset(im, 0, n * sizeof(im[0]));

	for (size_t i = 0; i < use; i++) {
		const size_t src = count - use + i;

		re[i] = mag_at(ctx, src) * hanning(i, use);
	}

	fft_in_place(re, im, n);

	{
		const size_t half = n / 2U;
		const float bin_hz = sample_hz / (float)n;
		const float band_hz = (sample_hz * 0.5f) / (float)VIBRO_BAND_COUNT;

		for (size_t b = 0; b < VIBRO_BAND_COUNT; b++) {
			const float f_lo = (float)b * band_hz;
			const float f_hi = (float)(b + 1U) * band_hz;
			size_t i_lo = (size_t)(f_lo / bin_hz);
			size_t i_hi = (size_t)(f_hi / bin_hz);

			if (i_lo < 1U) {
				i_lo = 1U;
			}
			if (i_hi <= i_lo) {
				i_hi = i_lo + 1U;
			}
			if (i_hi > half) {
				i_hi = half;
			}

			double sum_sq = 0.0;

			for (size_t i = i_lo; i < i_hi; i++) {
				const float mag =
					sqrtf(re[i] * re[i] + im[i] * im[i]) / (float)n;

				sum_sq += (double)mag * (double)mag;
			}

			const size_t bins = i_hi - i_lo;

			out.bands[b] = bins > 0U ? (float)sqrt(sum_sq / (double)bins) : 0.0f;
		}
	}

	out.valid = true;
	return out;
}
