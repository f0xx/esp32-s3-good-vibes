#pragma once

#include <stdbool.h>
#include <stdint.h>

int panel_backlight_init(void);
/** Re-attach PWM after BT/WiFi init clobbers LEDC (matches Arduino ensureBacklightOn). */
void panel_backlight_reapply(void);
void panel_backlight_set_on(bool on);
bool panel_backlight_is_on(void);
void panel_backlight_set_percent(uint8_t percent);
uint8_t panel_backlight_percent(void);
