#include "crash_debug.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(crash_dbg, LOG_LEVEL_INF);

#if defined(CONFIG_APP_CRASH_DEBUG)

static atomic_t g_inject_pending;
static char g_inject_kind[16];

static void stack_overflow_fn(void)
{
	stack_overflow_fn();
}

bool crash_debug_available(void)
{
	return true;
}

void crash_debug_inject(const char *kind)
{
	if (kind == NULL) {
		kind = "panic";
	}

	LOG_WRN("crash inject: %s", kind);

	if (strcmp(kind, "assert") == 0) {
		__ASSERT(false, "debug_inject assert");
		return;
	}
	if (strcmp(kind, "null") == 0) {
		volatile int *p = NULL;

		*p = 42;
		return;
	}
	if (strcmp(kind, "div0") == 0) {
		volatile int x = 42;
		volatile int y = 0;
		volatile int z = x / y;

		ARG_UNUSED(z);
		return;
	}
	if (strcmp(kind, "stack") == 0) {
		stack_overflow_fn();
		return;
	}
	if (strcmp(kind, "wdt") == 0) {
		while (true) {
			k_sleep(K_FOREVER);
		}
	}

	k_panic();
}

bool crash_debug_schedule_inject(const char *kind)
{
	if (kind == NULL || kind[0] == '\0') {
		return false;
	}

	strncpy(g_inject_kind, kind, sizeof(g_inject_kind) - 1U);
	g_inject_kind[sizeof(g_inject_kind) - 1U] = '\0';
	atomic_set(&g_inject_pending, 1);
	return true;
}

void crash_debug_poll(void)
{
	if (!atomic_cas(&g_inject_pending, 1, 0)) {
		return;
	}

	crash_debug_inject(g_inject_kind);
}

static int crash_debug_init(void)
{
	atomic_set(&g_inject_pending, 0);
	LOG_INF("crash debug inject enabled (non-production, main poll)");
	return 0;
}

SYS_INIT(crash_debug_init, APPLICATION, 90);

#else /* !CONFIG_APP_CRASH_DEBUG */

bool crash_debug_available(void)
{
	return false;
}

void crash_debug_inject(const char *kind)
{
	ARG_UNUSED(kind);
}

bool crash_debug_schedule_inject(const char *kind)
{
	ARG_UNUSED(kind);
	return false;
}

void crash_debug_poll(void)
{
}

#endif
