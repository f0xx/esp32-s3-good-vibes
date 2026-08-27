#include "vibro_fft.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

bool vibro_fft_uses_esp_dsp(void)
{
#if defined(CONFIG_VIBRO_ESP_DSP)
	return true;
#else
	return false;
#endif
}

float vibro_fft_hanning(size_t i, size_t n)
{
	if (n <= 1U) {
		return 1.0f;
	}
	return 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(n - 1U)));
}

void vibro_fft_in_place(float *re, float *im, size_t n)
{
#if defined(CONFIG_VIBRO_ESP_DSP)
	/* Future: dsps_fft2r_fc32 + dsps_bit_rev_fc32 when esp-dsp is in the west tree. */
#endif
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
