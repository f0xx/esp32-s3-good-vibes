#pragma once

/*
 * Minimal Zephyr atomic_t stand-in for host mocks (stdatomic).
 * Enough for imu_pipeline / BLE-style flags used under TSan.
 */

#include <stdatomic.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef atomic_int atomic_t;

#define ATOMIC_INIT(v) (v)

static inline int atomic_get(const atomic_t *a)
{
	return atomic_load(a);
}

static inline void atomic_set(atomic_t *a, int v)
{
	atomic_store(a, v);
}

static inline int atomic_cas(atomic_t *a, int old_val, int new_val)
{
	return atomic_compare_exchange_strong(a, &old_val, new_val) ? 1 : 0;
}

static inline int atomic_inc(atomic_t *a)
{
	return atomic_fetch_add(a, 1) + 1;
}

static inline int atomic_dec(atomic_t *a)
{
	return atomic_fetch_sub(a, 1) - 1;
}

static inline int atomic_or(atomic_t *a, int bits)
{
	return atomic_fetch_or(a, bits);
}

static inline int atomic_and(atomic_t *a, int bits)
{
	return atomic_fetch_and(a, bits);
}

static inline int atomic_clear_bit(atomic_t *a, int bit)
{
	return atomic_fetch_and(a, ~(1 << bit));
}

static inline int atomic_set_bit(atomic_t *a, int bit)
{
	return atomic_fetch_or(a, (1 << bit));
}

static inline int atomic_test_bit(const atomic_t *a, int bit)
{
	return (atomic_get(a) & (1 << bit)) != 0;
}

#ifdef __cplusplus
}
#endif
