#include "metrics_compact.h"

#include <math.h>

static uint32_t f32_to_bits(float f)
{
	union {
		float f;
		uint32_t u;
	} x = { .f = f };

	return x.u;
}

static float bits_to_f32(uint32_t u)
{
	union {
		uint32_t u;
		float f;
	} x = { .u = u };

	return x.f;
}

uint16_t metrics_f32_to_f16(float v)
{
	const uint32_t f = f32_to_bits(v);
	const uint32_t sign = (f >> 16) & 0x8000U;
	uint32_t exp = (f >> 23) & 0xffU;
	uint32_t mant = f & 0x7fffffU;

	if (exp == 255U) {
		return (uint16_t)(sign | 0x7c00U | (mant ? 0x0200U : 0U));
	}
	if (exp == 0U && mant == 0U) {
		return (uint16_t)sign;
	}

	int32_t new_exp = (int32_t)exp - 127 + 15;

	if (new_exp >= 31) {
		return (uint16_t)(sign | 0x7c00U);
	}
	if (new_exp <= 0) {
		if (new_exp < -10) {
			return (uint16_t)sign;
		}
		mant |= 0x800000U;
		mant >>= (uint32_t)(1 - new_exp);
		return (uint16_t)(sign | (mant >> 13));
	}

	return (uint16_t)(sign | ((uint32_t)new_exp << 10) | (mant >> 13));
}

float metrics_f16_to_f32(uint16_t bits)
{
	const uint32_t sign = (uint32_t)(bits & 0x8000U) << 16;
	const uint32_t exp = (bits >> 10) & 0x1fU;
	const uint32_t mant = bits & 0x3ffU;
	uint32_t out;

	if (exp == 0U) {
		if (mant == 0U) {
			out = sign;
		} else {
			uint32_t m = mant;
			int32_t e = -1;

			while ((m & 0x400U) == 0U) {
				m <<= 1;
				e--;
			}
			m &= 0x3ffU;
			out = sign | (uint32_t)((127 - 15 + 1 + e) << 23) | (m << 13);
		}
	} else if (exp == 31U) {
		out = sign | 0x7f800000U | (mant << 13);
	} else {
		out = sign | ((exp + 127U - 15U) << 23) | (mant << 13);
	}
	return bits_to_f32(out);
}

void metrics_f32_array_to_f16(const float *in, uint16_t *out, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		out[i] = metrics_f32_to_f16(in[i]);
	}
}
