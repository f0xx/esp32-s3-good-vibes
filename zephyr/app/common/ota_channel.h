#pragma once

#include <stdbool.h>
#include <stddef.h>

/** Preferred CDN channel for WiFi self-OTA (stable|staging|dev). Default: stable. */
#define OTA_CHANNEL_MAX 8

void ota_channel_init(void);

/** Normalized channel name always non-NULL ("stable" if unset/invalid). */
const char *ota_channel_get(void);

/** Persist preferred channel. Returns true if stored (or unchanged). */
bool ota_channel_set(const char *channel);

/**
 * Channel that supplied the last staged firmware image (BLE or WiFi).
 * Used in soft-crash fw_upgrade detail. Empty until first upgrade.
 */
const char *ota_channel_source_get(void);

/** Stamp the channel used for the in-flight / just-finished upgrade. */
void ota_channel_note_source(const char *channel);

/** Format short outcome+channel for crash ring (e.g. "ok:stable"). */
void ota_channel_format_outcome(char *dst, size_t dst_len, const char *outcome);
