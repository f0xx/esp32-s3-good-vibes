#include <string.h>

#include "panel_draw.h"
#include "panel_fb.h"
#include "panel_font_glcd.h"

#define GLYPH_COLS 5
#define GLYPH_ROWS 8
#define GLYPH_ADVANCE 6

void panel_draw_pixel(int16_t x, int16_t y, uint16_t color)
{
	panel_fb_put(x, y, color);
}

void panel_draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
	int16_t dx = x1 - x0;
	int16_t dy = y1 - y0;
	int16_t sx = (dx >= 0) ? 1 : -1;
	int16_t sy = (dy >= 0) ? 1 : -1;
	int16_t ax = (dx >= 0) ? dx : -dx;
	int16_t ay = (dy >= 0) ? dy : -dy;
	int16_t err = ax - ay;

	while (1) {
		panel_fb_put(x0, y0, color);
		if (x0 == x1 && y0 == y1) {
			break;
		}
		const int16_t e2 = err * 2;

		if (e2 > -ay) {
			err -= ay;
			x0 += sx;
		}
		if (e2 < ax) {
			err += ax;
			y0 += sy;
		}
	}
}

void panel_draw_circle(int16_t cx, int16_t cy, int16_t r, uint16_t color, bool filled)
{
	for (int16_t y = -r; y <= r; y++) {
		for (int16_t x = -r; x <= r; x++) {
			const int32_t d = x * x + y * y;

			if (d > r * r) {
				continue;
			}
			if (!filled && d < (r - 1) * (r - 1)) {
				continue;
			}
			panel_fb_put(cx + x, cy + y, color);
		}
	}
}

/*
 * TFT_eSPI / Adafruit_GFX font 1: 5 columns, bit0 = top row, 8 rows scanned.
 */
static void draw_glyph_upright(int16_t x, int16_t y, uint16_t color, const uint8_t *g,
			       uint8_t scale)
{
	for (uint8_t col = 0; col < GLYPH_COLS; col++) {
		uint8_t line = g[col];

		for (uint8_t row = 0; row < GLYPH_ROWS; row++, line >>= 1) {
			if ((line & 1U) == 0U) {
				continue;
			}
			for (uint8_t sy = 0; sy < scale; sy++) {
				for (uint8_t sx = 0; sx < scale; sx++) {
					panel_fb_put(x + (int16_t)col * scale + sx,
						     y + (int16_t)row * scale + sy, color);
				}
			}
		}
	}
}

/* Axis labels: 90° CCW for portrait panel GRAM orientation. */
static void draw_glyph_rotated(int16_t x, int16_t y, uint16_t color, const uint8_t *g,
			       uint8_t scale)
{
	for (uint8_t col = 0; col < GLYPH_COLS; col++) {
		uint8_t line = g[col];

		for (uint8_t row = 0; row < GLYPH_ROWS; row++, line >>= 1) {
			if ((line & 1U) == 0U) {
				continue;
			}
			for (uint8_t sy = 0; sy < scale; sy++) {
				for (uint8_t sx = 0; sx < scale; sx++) {
					panel_fb_put(x + (int16_t)row * scale + sx,
						     y + (int16_t)(GLYPH_COLS - 1U - col) * scale +
							     sy,
						     color);
				}
			}
		}
	}
}

void panel_draw_text(int16_t x, int16_t y, uint16_t color, const char *text, uint8_t scale)
{
	if (text == NULL || scale == 0U) {
		return;
	}

	int16_t cx = x;
	const int16_t advance = (int16_t)(GLYPH_ADVANCE * scale);

	for (size_t i = 0; text[i] != '\0'; i++) {
		draw_glyph_upright(cx, y, color, panel_font_glcd_glyph((uint8_t)text[i]), scale);
		cx += advance;
	}
}

void panel_draw_text_rotated(int16_t x, int16_t y, uint16_t color, const char *text,
			     uint8_t scale)
{
	if (text == NULL || scale == 0U) {
		return;
	}

	int16_t cx = x;
	const int16_t advance = (int16_t)(GLYPH_ROWS * scale);

	for (size_t i = 0; text[i] != '\0'; i++) {
		draw_glyph_rotated(cx, y, color, panel_font_glcd_glyph((uint8_t)text[i]), scale);
		cx += advance;
	}
}
