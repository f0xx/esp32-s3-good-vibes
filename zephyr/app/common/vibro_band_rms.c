#include "vibro_band_rms.h"

#include <math.h>
#include <string.h>

#include "vibro_fft.h"

struct vibro_band_rms vibro_band_rms_compute_series(size_t count, float sample_hz,
						    vibro_mag_at_fn mag_at, void *ctx)
{
	struct vibro_band_rms out = { 0 };
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

		re[i] = mag_at(ctx, src) * vibro_fft_hanning(i, use);
	}

	vibro_fft_in_place(re, im, n);

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
