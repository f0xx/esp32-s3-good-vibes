#include "mt200_host.h"

#include <zephyr/logging/log.h>

#include "ble_host.h"
#include "host_latency.h"

LOG_MODULE_REGISTER(mt200_host, LOG_LEVEL_INF);

static uint32_t g_events;
static bool g_did_hold;
static bool g_did_ignore_start;
static uint32_t g_hold_at_ms = 4500; /* med mt200 inter-event from ttyACM0 */

void mt200_host_init(void)
{
	host_latency_us(HOST_LAT_MT200_CALL_US);
	g_events = 0;
	g_did_hold = false;
	g_did_ignore_start = false;
	LOG_INF("mt200_host init (phone-hold script)");
}

void mt200_host_tick(uint32_t now_ms)
{
	host_latency_us(HOST_LAT_MT200_CALL_US);

	if (!g_did_hold && now_ms >= g_hold_at_ms && ble_host_connected()) {
		g_did_hold = true;
		g_events++;
		LOG_INF("MT200: phone hold — dropping central (host stub)");
	}
	if (!g_did_ignore_start && now_ms >= g_hold_at_ms + 4500U && ble_host_connected()) {
		g_did_ignore_start = true;
		g_events++;
		LOG_INF("MT200: start ignored (phone hold) (host stub)");
	}
}

uint32_t mt200_host_events(void)
{
	return g_events;
}
