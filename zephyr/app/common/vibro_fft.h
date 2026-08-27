#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Radix-2 in-place complex FFT (real + imag arrays, n power of 2). */
void vibro_fft_in_place(float *re, float *im, size_t n);

/** Hanning window sample. */
float vibro_fft_hanning(size_t i, size_t n);

/** True when built with esp-dsp backend (CONFIG_VIBRO_ESP_DSP). */
bool vibro_fft_uses_esp_dsp(void);
