#pragma once

#include <stdbool.h>
#include <stdint.h>

/** Non-fatal controlled reboot reasons (uploaded as soft reboot, not fatal crash). */
enum soft_reboot_kind {
	SOFT_REBOOT_NONE = 0,
	SOFT_REBOOT_BOOT_BTN,
	SOFT_REBOOT_RTS,
	SOFT_REBOOT_FW_UPGRADE,
};

/** Schedule a soft reboot record before sys_reboot(). Persists to NVS synchronously. */
void soft_reboot_schedule(enum soft_reboot_kind kind, uint8_t boot_part, uint8_t target_part);

/** Schedule with OTA outcome text in detail (e.g. transition_ok, target_non_operable). */
void soft_reboot_schedule_ota(enum soft_reboot_kind kind, uint8_t boot_part, uint8_t target_part,
			      const char *ota_outcome);

void soft_reboot_init(void);

/** After settings init: append pending soft reboot to crash ring; detect RTS if unscheduled. */
void soft_reboot_post_boot(void);

/** Running image partition label 0=A (slot0), 1=B (slot1), 255=unknown. */
uint8_t soft_reboot_boot_partition(void);

const char *soft_reboot_partition_label(uint8_t part);

const char *soft_reboot_kind_str(enum soft_reboot_kind kind);
