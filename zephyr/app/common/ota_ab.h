#pragma once

#include <stdbool.h>
#include <stdint.h>

struct flash_img_context;

#define OTA_AB_MAX_BOOT_TRIES 3U

void ota_ab_init(void);

/** Call early after settings init — handles mcuboot test/revert/retry state. */
void ota_ab_on_boot(void);

/** Call from main loop — confirm test image after stable uptime. */
void ota_ab_poll(void);

/** After BLE OTA flash complete: request test boot to inactive slot and reboot. */
int ota_ab_finish_and_reboot(struct flash_img_context *ctx);

bool ota_ab_target_marked_bad(uint8_t slot);
