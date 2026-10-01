#include "ble_host.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "host_debug.h"
#include "host_latency.h"

LOG_MODULE_REGISTER(ble_host, LOG_LEVEL_INF);

enum { BLE_FIFO_CAP = 32 };

static struct k_mutex g_lock;
static bool g_connected;
static bool g_notify;
static struct imu_sample g_fifo[BLE_FIFO_CAP];
static uint32_t g_fifo_t[BLE_FIFO_CAP];
static size_t g_head;
static size_t g_count;
static uint32_t g_drops;

static uint32_t g_script_connect_ms = HOST_LAT_BLE_SCRIPT_CONN_MS;
static uint32_t g_script_notify_ms = HOST_LAT_BLE_SCRIPT_NOTIFY_MS;
static bool g_script_armed = true;
static bool g_script_did_connect;
static bool g_script_did_notify;

void ble_host_init(void)
{
	host_latency_us(HOST_LAT_BLE_CALL_US);
	k_mutex_init(&g_lock);
	g_connected = false;
	g_notify = false;
	g_head = 0;
	g_count = 0;
	g_drops = 0;
	g_script_armed = true;
	g_script_did_connect = false;
	g_script_did_notify = false;
	LOG_INF("ble_host init (scripted phone stub)");
}

void ble_host_script_arm(uint32_t connect_after_ms, uint32_t notify_after_ms)
{
	host_latency_us(HOST_LAT_BLE_CALL_US);
	g_script_connect_ms = connect_after_ms;
	g_script_notify_ms = notify_after_ms;
	g_script_armed = true;
	g_script_did_connect = false;
	g_script_did_notify = false;
	LOG_INF("ble_host script arm connect@%ums notify@%ums", connect_after_ms,
		notify_after_ms);
}

void ble_host_set_connected(bool on)
{
	host_latency_us(HOST_LAT_BLE_CONN_US);
	k_mutex_lock(&g_lock, K_FOREVER);
	g_connected = on;
	if (!on) {
		g_notify = false;
	}
	k_mutex_unlock(&g_lock);
	LOG_INF("ble_host %s", on ? "connected" : "disconnected");
}

bool ble_host_connected(void)
{
	bool on;

	k_mutex_lock(&g_lock, K_FOREVER);
	on = g_connected;
	k_mutex_unlock(&g_lock);
	return on;
}

void ble_host_set_notify_enabled(bool on)
{
	bool enabled;

	host_latency_us(HOST_LAT_BLE_NOTIFY_US);
	k_mutex_lock(&g_lock, K_FOREVER);
	g_notify = on && g_connected;
	enabled = g_notify;
	k_mutex_unlock(&g_lock);
	LOG_INF("ble_host NOTIFY %s", enabled ? "enabled" : "disabled");
}

bool ble_host_notify_enabled(void)
{
	bool on;

	k_mutex_lock(&g_lock, K_FOREVER);
	on = g_notify;
	k_mutex_unlock(&g_lock);
	return on;
}

void ble_host_enqueue_sample(const struct imu_sample *s, uint32_t t_ms)
{
	if (s == NULL) {
		return;
	}

	host_latency_us(HOST_LAT_BLE_CALL_US);
	k_mutex_lock(&g_lock, K_FOREVER);
	if (!g_notify) {
		k_mutex_unlock(&g_lock);
		return;
	}
	if (g_count == BLE_FIFO_CAP) {
		g_head = (g_head + 1U) % BLE_FIFO_CAP;
		g_count--;
		g_drops++;
		HOST_LOG(HOST_LOG_DBG, "ble_host", "NOTIFY fifo drop (total=%u)", g_drops);
	}
	const size_t idx = (g_head + g_count) % BLE_FIFO_CAP;
	g_fifo[idx] = *s;
	g_fifo_t[idx] = t_ms;
	g_count++;
	k_mutex_unlock(&g_lock);
}

size_t ble_host_drain_notify(struct imu_sample *out, size_t max)
{
	size_t n = 0;

	if (out == NULL || max == 0) {
		return 0;
	}
	host_latency_us(HOST_LAT_BLE_DRAIN_US);
	k_mutex_lock(&g_lock, K_FOREVER);
	while (n < max && g_count > 0) {
		out[n++] = g_fifo[g_head];
		g_head = (g_head + 1U) % BLE_FIFO_CAP;
		g_count--;
	}
	k_mutex_unlock(&g_lock);
	return n;
}

uint32_t ble_host_notify_drops(void)
{
	uint32_t n;

	k_mutex_lock(&g_lock, K_FOREVER);
	n = g_drops;
	k_mutex_unlock(&g_lock);
	return n;
}

uint32_t ble_host_notify_queued(void)
{
	uint32_t n;

	k_mutex_lock(&g_lock, K_FOREVER);
	n = (uint32_t)g_count;
	k_mutex_unlock(&g_lock);
	return n;
}

void ble_host_script_tick(uint32_t now_ms)
{
	if (!g_script_armed) {
		return;
	}
	host_latency_us(HOST_LAT_BLE_CALL_US / 4U);
	if (!g_script_did_connect && now_ms >= g_script_connect_ms) {
		g_script_did_connect = true;
		ble_host_set_connected(true);
	}
	if (!g_script_did_notify && now_ms >= g_script_notify_ms) {
		g_script_did_notify = true;
		ble_host_set_notify_enabled(true);
	}
}
