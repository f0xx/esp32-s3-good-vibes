#include "network_manager.h"

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "net_profile_store.h"
#include "panel_backlight.h"
#include "clock_sync.h"
#include "power_manager.h"
#include "stall_watchdog.h"

LOG_MODULE_REGISTER(net_mgr, LOG_LEVEL_INF);

#if IS_ENABLED(CONFIG_WIFI)

#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>

#define MAX_SCAN_APS 20

struct scan_ap {
	char ssid[WIFI_SSID_MAX_LEN + 1];
	int8_t rssi;
	uint8_t sec;
};

static bool g_wifi_up;
static struct net_mgmt_event_callback mgmt_cb;
static struct net_if *wifi_iface;
static struct scan_ap g_aps[MAX_SCAN_APS];
static uint8_t g_ap_count;
static bool g_scanning;
static bool g_connected;
static char g_conn_ssid[33];
static int8_t g_conn_rssi = -127;
static int8_t g_conn_idx = -1;

static enum {
	PENDING_NONE = 0,
	PENDING_CONNECT_IDX,
	PENDING_CONNECT_CREDS,
} g_pending;
static uint8_t g_pending_idx;
static char g_pending_ssid[33];
static char g_pending_pass[65];

static network_wifi_status_fn g_wifi_status_cb;
static uint32_t g_connect_started_ms;
static int64_t g_connect_after_ms;
static int64_t g_wifi_settle_until_ms;
static bool g_scan_passive;

int __attribute__((weak)) esp32_wifi_radio_release(void)
{
	LOG_WRN("WiFi radio_release missing (driver patch not applied)");
	return -ENOTSUP;
}

static void run_pending_connect(void);
static void ensure_wifi_up(void);

static void notify_wifi_status(const char *state)
{
	if (g_wifi_status_cb != NULL) {
		g_wifi_status_cb(state);
	}
}

static int wifi_connect_req(const char *ssid, const char *pass);

static void run_pending_connect(void)
{
	power_manager_reapply_backlight_if_screen_on();
	/* Bring the MAC up only after BLE drop settle — starting it in
	 * connect_* raced phone teardown and starved net buffers. */
	ensure_wifi_up();
	g_connect_started_ms = k_uptime_get_32();

	if (g_pending == PENDING_CONNECT_IDX) {
		struct net_profile p;

		g_pending = PENDING_NONE;
		if (net_profile_store_get(g_pending_idx, &p)) {
			g_conn_idx = (int8_t)g_pending_idx;
			if (wifi_connect_req(p.ssid, p.pass) != 0) {
				g_connect_started_ms = 0;
				notify_wifi_status("failed");
			}
		} else {
			g_connect_started_ms = 0;
			notify_wifi_status("failed");
		}
		return;
	}

	if (g_pending == PENDING_CONNECT_CREDS) {
		char ssid[33];
		char pass[65];

		snprintf(ssid, sizeof(ssid), "%s", g_pending_ssid);
		snprintf(pass, sizeof(pass), "%s", g_pending_pass);
		g_pending = PENDING_NONE;
		g_conn_idx = (int8_t)net_profile_store_find_ssid(ssid);
		net_profile_store_upsert(ssid, pass, 0);
		if (wifi_connect_req(ssid, pass) != 0) {
			g_connect_started_ms = 0;
			notify_wifi_status("failed");
		}
	}
}

static void json_escape(const char *src, char *dst, size_t dst_len)
{
	size_t o = 0;

	if (src == NULL || dst_len == 0) {
		return;
	}
	for (size_t i = 0; src[i] != '\0' && o + 2 < dst_len; i++) {
		if (src[i] == '\\' || src[i] == '"') {
			dst[o++] = '\\';
		}
		dst[o++] = src[i];
	}
	dst[o] = '\0';
}

static void store_ap(const struct wifi_scan_result *res)
{
	if (g_ap_count >= MAX_SCAN_APS || res->ssid_length == 0) {
		return;
	}

	for (uint8_t i = 0; i < g_ap_count; i++) {
		if (strncmp(g_aps[i].ssid, (const char *)res->ssid, res->ssid_length) == 0) {
			return;
		}
	}

	struct scan_ap *ap = &g_aps[g_ap_count++];

	memcpy(ap->ssid, res->ssid, res->ssid_length);
	ap->ssid[res->ssid_length] = '\0';
	ap->rssi = res->rssi;
	ap->sec = (res->security == WIFI_SECURITY_TYPE_NONE) ? 0U : 1U;
	LOG_INF("WiFi AP \"%s\" rssi=%d sec=%u", ap->ssid, ap->rssi, ap->sec);
}

static void event_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
			  struct net_if *iface)
{
	ARG_UNUSED(cb);

	if (iface != wifi_iface) {
		return;
	}

	if (mgmt_event == NET_EVENT_WIFI_SCAN_RESULT) {
		store_ap((const struct wifi_scan_result *)cb->info);
	} else if (mgmt_event == NET_EVENT_WIFI_SCAN_DONE) {
		g_scanning = false;
		LOG_INF("WiFi scan done (%u APs)", g_ap_count);
	} else if (mgmt_event == NET_EVENT_WIFI_CONNECT_RESULT) {
		const struct wifi_status *st = (const struct wifi_status *)cb->info;

		if (st->status == WIFI_STATUS_CONN_SUCCESS) {
			g_connected = true;
			g_connect_started_ms = 0;
			g_conn_rssi = -127;
			for (uint8_t i = 0; i < g_ap_count; i++) {
				if (strcmp(g_aps[i].ssid, g_conn_ssid) == 0) {
					g_conn_rssi = g_aps[i].rssi;
					break;
				}
			}
			if (g_conn_idx >= 0) {
				net_profile_store_mark_ok((uint8_t)g_conn_idx);
			}
			power_manager_reapply_backlight_if_screen_on();
			LOG_INF("WiFi connected (%s) rssi=%d", g_conn_ssid, (int)g_conn_rssi);
			notify_wifi_status("connected");
			clock_sync_ntp_on_wifi_connected();
		} else {
			g_connected = false;
			g_conn_rssi = -127;
			g_connect_started_ms = 0;
			LOG_WRN("WiFi connect failed (%d)", st->status);
			notify_wifi_status("failed");
		}
	} else if (mgmt_event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		g_connected = false;
		g_conn_rssi = -127;
	}
}

static int wifi_connect_req(const char *ssid, const char *pass)
{
	struct wifi_connect_req_params params = { 0 };

	if (wifi_iface == NULL || ssid == NULL) {
		return -EINVAL;
	}

	params.ssid = (const uint8_t *)ssid;
	params.ssid_length = strlen(ssid);
	if (pass != NULL && pass[0] != '\0') {
		params.psk = (const uint8_t *)pass;
		params.psk_length = strlen(pass);
		params.security = WIFI_SECURITY_TYPE_PSK;
	} else {
		params.security = WIFI_SECURITY_TYPE_NONE;
	}
	params.channel = WIFI_CHANNEL_ANY;
	params.timeout = 15000;

	snprintf(g_conn_ssid, sizeof(g_conn_ssid), "%s", ssid);
	LOG_INF("WiFi connect req ssid=\"%s\" sec=%s", ssid,
		params.security == WIFI_SECURITY_TYPE_NONE ? "open" : "psk");
	return net_mgmt(NET_REQUEST_WIFI_CONNECT, wifi_iface, &params, sizeof(params));
}

static void ensure_wifi_up(void)
{
	if (wifi_iface == NULL || g_wifi_up) {
		return;
	}

	(void)net_if_up(wifi_iface);
	g_wifi_up = true;
	LOG_INF("WiFi interface up (on demand)");
}

void network_manager_init(void)
{
	net_profile_store_init();
	wifi_iface = net_if_get_first_wifi();

	if (wifi_iface == NULL) {
		LOG_ERR("No WiFi interface");
		return;
	}

	net_mgmt_init_event_callback(&mgmt_cb, event_handler,
				     NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE |
					     NET_EVENT_WIFI_CONNECT_RESULT |
					     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&mgmt_cb);
	LOG_INF("network manager ready");
}

void network_manager_set_wifi_status_cb(network_wifi_status_fn cb)
{
	g_wifi_status_cb = cb;
}

void network_manager_start(void)
{
	/* WiFi radio stays down until BLE net scan/connect (saves ~80mA). */
}

void network_manager_tick(void)
{
	if (g_connect_after_ms > 0 && k_uptime_get() >= g_connect_after_ms) {
		g_connect_after_ms = 0;
		run_pending_connect();
	}

	if (g_connect_started_ms != 0U && !g_connected &&
	    (k_uptime_get_32() - g_connect_started_ms) >= 25000U) {
		g_connect_started_ms = 0;
		LOG_WRN("WiFi connect timeout");
		notify_wifi_status("failed");
		/* Free the MAC so BLE can recover; next connect will restart it. */
		(void)esp32_wifi_radio_release();
		g_wifi_settle_until_ms = k_uptime_get() + 1500;
	}
}

bool network_manager_start_scan(void)
{
	/* Duplicate start while a scan is already running used to return false, which
	 * made ble_net resume MT200 on top of an in-flight WiFi scan and starve the
	 * main task WDT. Treat "already scanning" as success. */
	if (g_scanning) {
		return true;
	}

	const bool passive = g_scan_passive;

	g_scan_passive = false;
	if (wifi_iface == NULL) {
		return false;
	}

	ensure_wifi_up();
	g_ap_count = 0;
	g_scanning = true;

	struct wifi_scan_params params = { 0 };

	/* Do not set bands/dwell: an empty band_chan list with bands=2.4 GHz
	 * made some driver paths scan zero channels. Active uses IDF defaults
	 * (Arduino WiFi.scanNetworks). */
	params.scan_type = passive ? WIFI_SCAN_TYPE_PASSIVE : WIFI_SCAN_TYPE_ACTIVE;

	/* Single attempt — do not k_sleep on the main loop (MAIN_WDT_MS is 15 s).
	 * Callers retry via g_scan_after_ms. */
	stall_watchdog_feed_main();
	const int err = net_mgmt(NET_REQUEST_WIFI_SCAN, wifi_iface, &params, sizeof(params));
	stall_watchdog_feed_main();
	if (err == 0) {
		LOG_INF("WiFi scan started (%s, 2.4 GHz)", passive ? "passive listen" : "active probe");
		return true;
	}

	LOG_WRN("WiFi scan request failed (%d)", err);
	g_scanning = false;
	return false;
}

void network_manager_request_passive_scan(void)
{
	g_scan_passive = true;
}

bool network_manager_connect_index(uint8_t idx)
{
	if (g_pending != PENDING_NONE || g_connect_started_ms != 0U) {
		return false;
	}

	g_pending_idx = idx;
	g_pending = PENDING_CONNECT_IDX;
	/* Soft coexist settle — stretch LE, then assoc (no phone disconnect). */
	g_connect_after_ms = k_uptime_get() + 400;
	return true;
}

bool network_manager_connect_creds(const char *ssid, const char *pass)
{
	if (ssid == NULL || g_pending != PENDING_NONE || g_connect_started_ms != 0U) {
		return false;
	}

	snprintf(g_pending_ssid, sizeof(g_pending_ssid), "%s", ssid);
	snprintf(g_pending_pass, sizeof(g_pending_pass), "%s", pass ? pass : "");
	g_pending = PENDING_CONNECT_CREDS;
	g_connect_after_ms = k_uptime_get() + 400;
	return true;
}

void network_manager_build_scan_json(char *dst, size_t dst_len)
{
	const int8_t active = net_profile_store_last_ok_index();
	int o = snprintf(dst, dst_len, "{\"aps\":[");

	for (uint8_t i = 0; i < g_ap_count && o > 0; i++) {
		char esc[40];
		const int idx = net_profile_store_find_ssid(g_aps[i].ssid);

		json_escape(g_aps[i].ssid, esc, sizeof(esc));
		if (i > 0) {
			o += snprintf(dst + o, dst_len - (size_t)o, ",");
		}
		o += snprintf(dst + o, dst_len - (size_t)o,
			      "{\"ssid\":\"%s\",\"rssi\":%d,\"sec\":%u,\"cfg\":%u,\"idx\":%d,"
			      "\"active\":%u}",
			      esc, g_aps[i].rssi, g_aps[i].sec, idx >= 0 ? 1U : 0U, idx,
			      (idx >= 0 && idx == active) ? 1U : 0U);
	}
	if (o > 0) {
		snprintf(dst + o, dst_len - (size_t)o,
			 "],\"n_profiles\":%u,\"last_ok\":%d,\"scanning\":%u}",
			 net_profile_store_count(), (int)active, g_scanning ? 1U : 0U);
	}
}

void network_manager_build_profiles_json(char *dst, size_t dst_len)
{
	const uint8_t n = net_profile_store_count();
	const int8_t active = net_profile_store_last_ok_index();
	int o = snprintf(dst, dst_len, "{\"profiles\":[");

	for (uint8_t i = 0; i < n && o > 0; i++) {
		struct net_profile p;

		if (!net_profile_store_get(i, &p)) {
			continue;
		}
		char esc[40];

		json_escape(p.ssid, esc, sizeof(esc));
		if (i > 0) {
			o += snprintf(dst + o, dst_len - (size_t)o, ",");
		}
		o += snprintf(dst + o, dst_len - (size_t)o,
			      "{\"idx\":%u,\"ssid\":\"%s\",\"last_ok\":%lu,\"active\":%u}", i, esc,
			      (unsigned long)p.last_ok_ms, active == (int8_t)i ? 1U : 0U);
	}
	if (o > 0) {
		snprintf(dst + o, dst_len - (size_t)o, "]}");
	}
}

void network_manager_build_status_json(char *dst, size_t dst_len, const char *state)
{
	char esc[40] = "";

	json_escape(g_conn_ssid, esc, sizeof(esc));
	snprintf(dst, dst_len,
		 "{\"st\":\"%s\",\"ssid\":\"%s\",\"idx\":%d,\"wifi\":%d,\"rssi\":0,\"ip\":\"\","
		 "\"portal\":0,\"profiles\":%u}",
		 state ? state : "idle", esc, (int)g_conn_idx, g_connected ? 1 : 0,
		 net_profile_store_count());
}

bool network_manager_scan_busy(void)
{
	return g_scanning;
}

uint8_t network_manager_ap_count(void)
{
	return g_ap_count;
}

bool network_manager_radio_busy(void)
{
	/* Include g_pending: connect is scheduled 200ms out; without this,
	 * radio_scheduler_sync() briefly clears wifi-busy and BLE restores
	 * mid-handshake (see "restore after WiFi" then net-buffer OOM). */
	if (g_scanning || g_pending != PENDING_NONE || g_connect_started_ms != 0U) {
		return true;
	}
	if (g_wifi_settle_until_ms > 0 && k_uptime_get() < g_wifi_settle_until_ms) {
		return true;
	}
	return false;
}

void network_manager_release_scan_radio(void)
{
	if (g_scanning || g_pending != PENDING_NONE || g_connected ||
	    g_connect_started_ms != 0U) {
		return;
	}

	(void)esp32_wifi_radio_release();
	g_wifi_settle_until_ms = k_uptime_get() + 2500;
}

bool network_manager_portal_active(void)
{
	return false;
}

bool network_manager_link_up(void)
{
	return g_connected;
}

int8_t network_manager_wifi_rssi(void)
{
	return g_connected ? g_conn_rssi : (int8_t)-127;
}

size_t network_manager_wifi_ssid(char *dst, size_t dst_len)
{
	if (dst == NULL || dst_len == 0U) {
		return 0U;
	}
	if (!g_connected || g_conn_ssid[0] == '\0') {
		dst[0] = '\0';
		return 0U;
	}
	snprintf(dst, dst_len, "%s", g_conn_ssid);
	return strlen(dst);
}

#else /* !CONFIG_WIFI */

void network_manager_set_wifi_status_cb(network_wifi_status_fn cb)
{
	ARG_UNUSED(cb);
}

void network_manager_init(void)
{
	net_profile_store_init();
	LOG_INF("network manager (profiles only — WiFi stack disabled at boot)");
}

void network_manager_start(void)
{
}

void network_manager_tick(void)
{
}

bool network_manager_start_scan(void)
{
	return false;
}

void network_manager_request_passive_scan(void)
{
}

bool network_manager_connect_index(uint8_t idx)
{
	ARG_UNUSED(idx);
	return false;
}

bool network_manager_connect_creds(const char *ssid, const char *pass)
{
	ARG_UNUSED(ssid);
	ARG_UNUSED(pass);
	return false;
}

void network_manager_build_scan_json(char *dst, size_t dst_len)
{
	snprintf(dst, dst_len, "{\"aps\":[],\"n_profiles\":%u,\"last_ok\":%d}",
		 net_profile_store_count(), (int)net_profile_store_last_ok_index());
}

void network_manager_build_profiles_json(char *dst, size_t dst_len)
{
	const uint8_t n = net_profile_store_count();
	const int8_t active = net_profile_store_last_ok_index();
	int o = snprintf(dst, dst_len, "{\"profiles\":[");

	for (uint8_t i = 0; i < n && o > 0; i++) {
		struct net_profile p;

		if (!net_profile_store_get(i, &p)) {
			continue;
		}
		if (i > 0) {
			o += snprintf(dst + o, dst_len - (size_t)o, ",");
		}
		o += snprintf(dst + o, dst_len - (size_t)o,
			      "{\"idx\":%u,\"ssid\":\"%s\",\"last_ok\":%lu,\"active\":%u}", i,
			      p.ssid, (unsigned long)p.last_ok_ms, active == (int8_t)i ? 1U : 0U);
	}
	if (o > 0) {
		snprintf(dst + o, dst_len - (size_t)o, "]}");
	}
}

void network_manager_build_status_json(char *dst, size_t dst_len, const char *state)
{
	snprintf(dst, dst_len,
		 "{\"st\":\"%s\",\"msg\":\"wifi disabled\",\"wifi\":0,\"portal\":0,\"profiles\":%u}",
		 state ? state : "idle", net_profile_store_count());
}

bool network_manager_scan_busy(void)
{
	return false;
}

uint8_t network_manager_ap_count(void)
{
	return 0;
}

bool network_manager_radio_busy(void)
{
	return false;
}

void network_manager_release_scan_radio(void)
{
}

bool network_manager_portal_active(void)
{
	return false;
}

bool network_manager_link_up(void)
{
	return false;
}

int8_t network_manager_wifi_rssi(void)
{
	return -127;
}

size_t network_manager_wifi_ssid(char *dst, size_t dst_len)
{
	if (dst != NULL && dst_len > 0U) {
		dst[0] = '\0';
	}
	return 0U;
}

#endif /* CONFIG_WIFI */
