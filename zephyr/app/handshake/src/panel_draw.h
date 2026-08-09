#pragma once

#include <stdbool.h>
#include <stdint.h>

void panel_draw_pixel(int16_t x, int16_t y, uint16_t color);
void panel_draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
void panel_draw_circle(int16_t cx, int16_t cy, int16_t r, uint16_t color, bool filled);
/** Upright TFT_eSPI font 1 (uniform scale, 6px advance at scale 1). */
void panel_draw_text(int16_t x, int16_t y, uint16_t color, const char *text, uint8_t scale);
/** 90° CCW — axis labels on portrait panel. */
void panel_draw_text_rotated(int16_t x, int16_t y, uint16_t color, const char *text,
			     uint8_t scale);
