#include "wifi_host.h"

#include <zephyr/logging/log.h>

#include "host_latency.h"

LOG_MODULE_REGISTER(wifi_host, LOG_LEVEL_INF);

static bool g_ready;

void wifi_host_init(void)
{
	host_latency_us(HOST_LAT_WIFI_CALL_US);
	g_ready = true;
	LOG_INF("net_mgr: network manager ready (host stub)");
	host_latency_us(HOST_LAT_WIFI_CALL_US);
	LOG_INF("ota_wifi: wifi ota disabled (host stub)");
}

void wifi_host_tick(void)
{
	host_latency_us(HOST_LAT_WIFI_CALL_US / 5U);
}

bool wifi_host_ready(void)
{
	return g_ready;
}
