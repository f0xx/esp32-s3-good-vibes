#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ble_net_protocol.h"

typedef void (*network_wifi_status_fn)(const char *state);

void network_manager_init(void);
void network_manager_start(void);
void network_manager_tick(void);
void network_manager_set_wifi_status_cb(network_wifi_status_fn cb);

bool network_manager_start_scan(void);
/** Next start_scan uses passive listen (beacons only — better with BLE on the antenna). */
void network_manager_request_passive_scan(void);
bool network_manager_connect_index(uint8_t idx);
bool network_manager_connect_creds(const char *ssid, const char *pass);
void network_manager_build_scan_json(char *dst, size_t dst_len);
void network_manager_build_profiles_json(char *dst, size_t dst_len);
void network_manager_build_status_json(char *dst, size_t dst_len, const char *state);

bool network_manager_scan_busy(void);
uint8_t network_manager_ap_count(void);
/** True while WiFi scan or connect is in progress — pause heavy BLE IMU traffic. */
bool network_manager_radio_busy(void);
/** Stop the ESP32 WiFi MAC after a scan so BLE HCI can recover. No-op if STA is connecting. */
void network_manager_release_scan_radio(void);
bool network_manager_portal_active(void);
/** True after STA got an association (not merely iface-up). */
bool network_manager_link_up(void);
/** Connected SSID RSSI (dBm), or -127 if unknown / down. */
int8_t network_manager_wifi_rssi(void);
/** Copy connected SSID (empty if down). Returns length. */
size_t network_manager_wifi_ssid(char *dst, size_t dst_len);
