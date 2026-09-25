#include "host_latency.h"

#include <stdlib.h>

#include <zephyr/kernel.h>

#include "host_debug.h"

static bool g_enabled = true;

void host_latency_init(void)
{
	const char *env = getenv("HOST_NO_LATENCY");

	g_enabled = !(env && env[0] == '1');
	HOST_LOG(HOST_LOG_INF, "host_latency", "stub delays %s (from ttyACM0.log avgs)",
		 g_enabled ? "ON" : "OFF");
}

bool host_latency_enabled(void)
{
	return g_enabled;
}

void host_latency_us(uint32_t us)
{
	if (!g_enabled || us == 0U) {
		return;
	}
	k_usleep((int32_t)us);
}

void host_latency_ms(uint32_t ms)
{
	if (!g_enabled || ms == 0U) {
		return;
	}
	k_msleep((int32_t)ms);
}
