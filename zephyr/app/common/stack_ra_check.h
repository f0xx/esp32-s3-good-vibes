#pragma once

/*
 * Cheap return-address canaries (manual stack-smash bisect).
 * Technique: https://rkd.me.uk/posts/2020-04-11-stack-corruption-and-how-to-debug-it.html
 *
 * STACK_RA_CHECK_SETUP at function entry; STACK_RA_CHECK() after risky work.
 * On mismatch → printk + k_oops (RTC fatal capture on ESP32-S3).
 *
 * Enabled with CONFIG_APP_STACK_RA_CHECK (desk prj_crash). No-ops otherwise.
 */

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#if IS_ENABLED(CONFIG_APP_STACK_RA_CHECK)

#define STACK_RA_CHECK_SETUP                                                               \
	const void *const _stack_ra_saved = __builtin_return_address(0)

#define STACK_RA_CHECK()                                                                   \
	do {                                                                               \
		const void *const _stack_ra_now = __builtin_return_address(0);                \
		if (_stack_ra_now != _stack_ra_saved) {                                    \
			printk("STACK_RA smash %s:%d saved=%p now=%p\n", __func__, __LINE__, \
			       _stack_ra_saved, _stack_ra_now);                              \
			k_oops();                                                          \
		}                                                                          \
	} while (0)

/* Nested scopes (e.g. loops / helpers inside a checked fn) — re-capture RA. */
#define STACK_RA_CHECK_REBASE                                                              \
	const void *const _stack_ra_saved = __builtin_return_address(0)

#else /* !CONFIG_APP_STACK_RA_CHECK */

#define STACK_RA_CHECK_SETUP                                                               \
	do {                                                                               \
	} while (0)
#define STACK_RA_CHECK()                                                                   \
	do {                                                                               \
	} while (0)
#define STACK_RA_CHECK_REBASE                                                              \
	do {                                                                               \
	} while (0)

#endif /* CONFIG_APP_STACK_RA_CHECK */
