#pragma once

#include <stdbool.h>

/*
 * True if it is currently safe to perform a flash_area_erase() call.
 *
 * Real hardware testing (ESP32-S3, octal PSRAM) showed that flash_area_erase()'s
 * interrupts/cache-disabled window silently resets BOTH cores (TG0WDT_SYS_RST, no
 * preceding fatal-exception log) if a BLE connection is live at essentially ANY point
 * during the erase — not just near the connection-setup/grace-elapse boundaries first
 * suspected. Every flash-backed store in this app (crash_ring_store, vibro_ref_store,
 * vibro_verdict_store, device_config) must therefore defer its erase-triggering
 * operations until this returns true, instead of doing them synchronously from a BLE
 * GATT write callback or a background poll that can run while connected.
 *
 * Defaults to always-safe here so common/ code (and any host-side tests) don't need to
 * link the BLE stack. The handshake app overrides this with a strong definition (see
 * ble_imu_gatt.c) that reflects real connection state via ble_imu_disconnected_settled().
 */
bool app_flash_erase_safe(void);
