#pragma once

#include <stdint.h>

#include <zephyr/device.h>

#include "display_panel.h"

/* Off-screen RGB565 buffer — render in RAM, one SPI flush per frame (Arduino sprite parity). */
void panel_fb_begin(uint16_t clear_color);
void panel_fb_put(int16_t x, int16_t y, uint16_t color);
void panel_fb_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void panel_fb_flush(const struct device *display);
uint32_t panel_fb_last_flush_ms(void);
/** True while SPI flush or panel blanking holds the display mutex. */
bool panel_display_busy(void);

/** Thread-safe display blanking; backlight is driven outside the flush mutex. */
bool panel_display_hw_set(const struct device *display, bool on);
