#pragma once

#include <stddef.h>
#include <stdint.h>

/** IEEE754 float32 → float16 bits (round-to-nearest). */
uint16_t metrics_f32_to_f16(float v);

/** float16 bits → float32. */
float metrics_f16_to_f32(uint16_t bits);

/** Encode `count` floats to `count` uint16 f16 in `out`. */
void metrics_f32_array_to_f16(const float *in, uint16_t *out, size_t count);
