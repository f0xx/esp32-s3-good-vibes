/*
 * ST7789 on Waveshare 1.47B — RGB565 with R/B fields swapped vs naive RGB.
 * Matches TFT_RGB_ORDER TFT_BGR in esp32_s3_imu_basics/tft_setup.h.
 *
 * GRAM is not cleared on MCU reset — always full-screen clear on boot or stale
 * bars from older flashes remain visible (0xFFE0 reads as pink/magenta).
 */
#pragma once

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>

#define PANEL_W      172
#define PANEL_H      320
#define PANEL_STRIP  16

#define PANEL_BLACK  0x0000
#define PANEL_WHITE  0xFFFF
#define PANEL_RED    0xF800
#define PANEL_GREEN  0x001F
#define PANEL_BLUE   0x07E0
#define PANEL_GREY   0x7BEF  /* lower-left marker — visible on black */
#define PANEL_YELLOW 0x07FF  /* not 0xFFE0 — byte order renders that as pink */

#define PANEL_BAR_H  24

void panel_fill_rect(const struct device *display, uint16_t x, uint16_t y, uint16_t w,
		     uint16_t h, uint16_t color);
void panel_clear_screen(const struct device *display, uint16_t color);
void panel_draw_corners(const struct device *display);
void panel_boot_bar(const struct device *display, uint16_t color);
