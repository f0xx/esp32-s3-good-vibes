#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int ble_ota_gatt_init(void);
/** Drive slot pre-erase / FAIL hold / delayed reboot (call from main loop). */
void ble_ota_gatt_tick(void);

/** True while OTA UI owns the panel (upgrade / FAIL hold / REBOOT). */
bool ble_ota_ui_active(void);

/**
 * Snapshot for the OTA panel. Returns false when idle (caller draws cube).
 * progress_permille: 0..1000. label is a static string (FAIL / REBOOT / Upgrading…).
 * label_rgb565 / bar_rgb565: panel colors.
 */
bool ble_ota_ui_snapshot(const char **label, uint16_t *label_rgb565, uint16_t *bar_rgb565,
			 uint16_t *progress_permille);
