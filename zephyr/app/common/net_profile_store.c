#include "net_profile_store.h"

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "board_config.h"

static struct net_profile g_profiles[NET_PROFILE_MAX];
static uint8_t g_count;
static int8_t g_last_ok = -1;

void net_profile_store_init(void)
{
	memset(g_profiles, 0, sizeof(g_profiles));
	g_count = 0;
	g_last_ok = -1;
}

uint8_t net_profile_store_count(void)
{
	return g_count;
}

bool net_profile_store_get(uint8_t idx, struct net_profile *out)
{
	if (out == NULL || idx >= g_count) {
		return false;
	}

	*out = g_profiles[idx];
	return out->ssid[0] != '\0';
}

int net_profile_store_find_ssid(const char *ssid)
{
	if (ssid == NULL) {
		return -1;
	}
	for (uint8_t i = 0; i < g_count; i++) {
		if (strcmp(g_profiles[i].ssid, ssid) == 0) {
			return (int)i;
		}
	}
	return -1;
}

int8_t net_profile_store_last_ok_index(void)
{
	return g_last_ok;
}

bool net_profile_store_upsert(const char *ssid, const char *pass, uint8_t method)
{
	if (ssid == NULL || ssid[0] == '\0') {
		return false;
	}

	int idx = net_profile_store_find_ssid(ssid);
	struct net_profile *p;

	if (idx >= 0) {
		p = &g_profiles[idx];
	} else {
		if (g_count >= NET_PROFILE_MAX) {
			return false;
		}
		idx = g_count++;
		p = &g_profiles[idx];
		memset(p, 0, sizeof(*p));
	}

	snprintf(p->ssid, sizeof(p->ssid), "%s", ssid);
	if (pass != NULL) {
		snprintf(p->pass, sizeof(p->pass), "%s", pass);
	}
	p->method = method;
	return true;
}

bool net_profile_store_remove_index(uint8_t idx)
{
	if (idx >= g_count) {
		return false;
	}

	for (uint8_t i = idx; i + 1U < g_count; i++) {
		g_profiles[i] = g_profiles[i + 1U];
	}
	memset(&g_profiles[g_count - 1U], 0, sizeof(g_profiles[0]));
	g_count--;
	if (g_last_ok == (int8_t)idx) {
		g_last_ok = -1;
	} else if (g_last_ok > (int8_t)idx) {
		g_last_ok--;
	}
	return true;
}

bool net_profile_store_remove_ssid(const char *ssid)
{
	const int idx = net_profile_store_find_ssid(ssid);

	if (idx < 0) {
		return false;
	}
	return net_profile_store_remove_index((uint8_t)idx);
}

void net_profile_store_mark_ok(uint8_t idx)
{
	if (idx >= g_count) {
		return;
	}

	g_profiles[idx].last_ok_ms = k_uptime_get_32();
	g_last_ok = (int8_t)idx;
}

void net_profile_store_clear_all(void)
{
	memset(g_profiles, 0, sizeof(g_profiles));
	g_count = 0;
	g_last_ok = -1;
}
