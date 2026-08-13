#include "clock_sync.h"

#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(clock_sync, LOG_LEVEL_INF);

static int64_t g_epoch_offset_ms;
static bool g_synced;
static enum clock_sync_source g_source;
static int16_t g_tz_offset_min;
static int64_t g_last_drift_ms;
static int64_t g_last_corr_ms;

static bool apply_wall_clock(int64_t unix_ms, int16_t tz_min, enum clock_sync_source src)
{
	const int64_t drift = g_synced ? clock_sync_drift_ms(unix_ms) : 0;
	const int64_t corr = drift >= 0 ? drift : -drift;

	g_epoch_offset_ms = unix_ms - (int64_t)k_uptime_get();
	g_synced = true;
	g_source = src;
	g_tz_offset_min = tz_min;
	g_last_corr_ms = corr;

	LOG_INF("TIME apply src=%u unix_ms=%lld tz=%d min drift=%lld ms corr=%lld ms",
		(unsigned)src, (long long)unix_ms, (int)tz_min, (long long)g_last_drift_ms,
		(long long)g_last_corr_ms);

	clock_sync_ntp_reset_schedule();
	return true;
}

void clock_sync_begin(void)
{
	g_epoch_offset_ms = 0;
	g_synced = false;
	g_source = CLOCK_SYNC_SRC_NONE;
	g_tz_offset_min = 0;
	g_last_drift_ms = 0;
	g_last_corr_ms = 0;
}

int64_t clock_sync_drift_ms(int64_t reference_unix_ms)
{
	if (reference_unix_ms < 1000000000000LL) {
		return 0;
	}

	return reference_unix_ms - clock_sync_now_ms();
}

bool clock_sync_set_from_phone(int64_t unix_ms, int16_t tz_offset_min)
{
	if (unix_ms < 1000000000000LL) {
		return false;
	}

	/* Before the first-ever sync, clock_sync_now_ms() returns raw uptime, not wall clock —
	 * diffing against it would print a meaningless multi-year "drift". */
	g_last_drift_ms = g_synced ? clock_sync_drift_ms(unix_ms) : 0;

	if (g_synced && g_last_drift_ms > -CLOCK_SYNC_DRIFT_APPLY_MS &&
	    g_last_drift_ms < CLOCK_SYNC_DRIFT_APPLY_MS) {
		if (g_tz_offset_min != tz_offset_min) {
			g_tz_offset_min = tz_offset_min;
			LOG_INF("TIME tz-only update tz=%d min drift=%lld ms (within threshold)",
				(int)tz_offset_min, (long long)g_last_drift_ms);
		}
#if defined(CONFIG_APP_CRASH_DEBUG)
		LOG_DBG("TIME phone skip drift=%lld ms", (long long)g_last_drift_ms);
#endif
		return false;
	}

	return apply_wall_clock(unix_ms, tz_offset_min, CLOCK_SYNC_SRC_PHONE);
}

bool clock_sync_set_from_ntp(int64_t unix_ms)
{
	if (unix_ms < 1000000000000LL) {
		return false;
	}

	g_last_drift_ms = g_synced ? clock_sync_drift_ms(unix_ms) : 0;
	return apply_wall_clock(unix_ms, 0, CLOCK_SYNC_SRC_NTP);
}

bool clock_sync_is_synced(void)
{
	return g_synced;
}

enum clock_sync_source clock_sync_source(void)
{
	return g_source;
}

int16_t clock_sync_tz_offset_min(void)
{
	return g_tz_offset_min;
}

int64_t clock_sync_last_drift_ms(void)
{
	return g_last_drift_ms;
}

int64_t clock_sync_last_corr_ms(void)
{
	return g_last_corr_ms;
}

int64_t clock_sync_now_ms(void)
{
	if (!g_synced) {
		return (int64_t)k_uptime_get();
	}

	return g_epoch_offset_ms + (int64_t)k_uptime_get();
}

uint32_t clock_sync_now_ms32(void)
{
	return (uint32_t)(clock_sync_now_ms() & 0xFFFFFFFFLL);
}

uint32_t clock_sync_now_unix_sec(void)
{
	return (uint32_t)(clock_sync_now_ms() / 1000LL);
}

int clock_sync_append_status_json(char *buf, size_t cap, int written)
{
	if (buf == NULL || written < 0 || (size_t)written >= cap) {
		return written;
	}

	const int n = snprintf(buf + written, cap - (size_t)written,
			       ",\"clks\":%u,\"clksrc\":%u,\"tz\":%d,\"clkd\":%lld,\"clkc\":%lld,"
			       "\"clku\":%u",
			       g_synced ? 1U : 0U, (unsigned)g_source, (int)g_tz_offset_min,
			       (long long)g_last_drift_ms, (long long)g_last_corr_ms,
			       clock_sync_now_unix_sec());

	if (n < 0) {
		return written;
	}

	if ((size_t)n >= cap - (size_t)written) {
		return (int)cap;
	}

	return written + n;
}
