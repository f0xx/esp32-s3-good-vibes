#pragma once

#include <stdbool.h>

/** WiFi CDN self-OTA (HTTPS). No-op until STA link is up and a profile exists. */
void ota_wifi_init(void);
void ota_wifi_poll(void);
bool ota_wifi_busy(void);
