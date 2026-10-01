#pragma once

#include <stdbool.h>
#include <stdint.h>

int panel_backlight_init(void);
/** Re-attach PWM after BT/WiFi init clobbers LEDC (matches Arduino ensureBacklightOn). */
void panel_backlight_reapply(void);
void panel_backlight_set_on(bool on);
bool panel_backlight_is_on(void);
/** Force PWM to 0 without changing the logical on/off. Used while LED debug
 *  is open so LCD white BL cannot mix into the acrylic. Resume restores g_on. */
void panel_backlight_hold_off(bool hold);
void panel_backlight_set_percent(uint8_t percent);
uint8_t panel_backlight_percent(void);
