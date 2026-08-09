#pragma once

#include <stdbool.h>
#include <stdint.h>

struct net_profile {
	char ssid[33];
	char pass[65];
	uint32_t last_ok_ms;
	uint8_t method;
};

void net_profile_store_init(void);
uint8_t net_profile_store_count(void);
bool net_profile_store_get(uint8_t idx, struct net_profile *out);
bool net_profile_store_upsert(const char *ssid, const char *pass, uint8_t method);
bool net_profile_store_remove_index(uint8_t idx);
bool net_profile_store_remove_ssid(const char *ssid);
int net_profile_store_find_ssid(const char *ssid);
int8_t net_profile_store_last_ok_index(void);
void net_profile_store_mark_ok(uint8_t idx);
void net_profile_store_clear_all(void);
