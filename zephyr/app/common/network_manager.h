#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ble_net_protocol.h"

typedef void (*network_wifi_status_fn)(const char *state);

void network_manager_init(void);
void network_manager_start(void);
void network_manager_tick(void);
void network_manager_set_wifi_status_cb(network_wifi_status_fn cb);

bool network_manager_start_scan(void);
bool network_manager_connect_index(uint8_t idx);
bool network_manager_connect_creds(const char *ssid, const char *pass);
void network_manager_build_scan_json(char *dst, size_t dst_len);
void network_manager_build_profiles_json(char *dst, size_t dst_len);
void network_manager_build_status_json(char *dst, size_t dst_len, const char *state);

bool network_manager_scan_busy(void);
/** True while WiFi scan or connect is in progress — pause heavy BLE IMU traffic. */
bool network_manager_radio_busy(void);
bool network_manager_portal_active(void);
