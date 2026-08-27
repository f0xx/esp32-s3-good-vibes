#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "display_panel.h"
#include "panel_draw.h"
#include "panel_fb.h"
#include "panel_font_glcd.h"

#define GLYPH_COLS 5
#define GLYPH_ROWS 8
#define GLYPH_ADVANCE 6

#define CS_LEFT   1
#define CS_RIGHT  2
#define CS_TOP    4
#define CS_BOTTOM 8

void panel_draw_pixel(int16_t x, int16_t y, uint16_t color)
{
	panel_fb_put(x, y, color);
}

static int clip_outcode(int32_t x, int32_t y)
{
	int code = 0;

	if (x < 0) {
		code |= CS_LEFT;
	} else if (x >= PANEL_W) {
		code |= CS_RIGHT;
	}
	if (y < 0) {
		code |= CS_TOP;
	} else if (y >= PANEL_H) {
		code |= CS_BOTTOM;
	}
	return code;
}

/* Cohen–Sutherland — keep Bresenham inside the panel so a near-plane exploded
 * projected endpoint cannot paint a 30k-pixel line (or wrap int16) while the
 * HUD strip still flushes. */
static bool clip_to_panel(int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1)
{
	int c0 = clip_outcode(*x0, *y0);
	int c1 = clip_outcode(*x1, *y1);

	int guard = 8;

	while (guard-- > 0) {
		if ((c0 | c1) == 0) {
			return true;
		}
		if ((c0 & c1) != 0) {
			return false;
		}

		const int c = (c0 != 0) ? c0 : c1;
		const int32_t dx = *x1 - *x0;
		const int32_t dy = *y1 - *y0;
		int32_t x = *x0;
		int32_t y = *y0;

		if ((c & CS_LEFT) != 0) {
			y = *y0 + (int32_t)((int64_t)dy * (0 - *x0) / (dx != 0 ? dx : 1));
			x = 0;
		} else if ((c & CS_RIGHT) != 0) {
			y = *y0 + (int32_t)((int64_t)dy * ((PANEL_W - 1) - *x0) /
					     (dx != 0 ? dx : 1));
			x = PANEL_W - 1;
		} else if ((c & CS_TOP) != 0) {
			x = *x0 + (int32_t)((int64_t)dx * (0 - *y0) / (dy != 0 ? dy : 1));
			y = 0;
		} else {
			x = *x0 + (int32_t)((int64_t)dx * ((PANEL_H - 1) - *y0) /
					     (dy != 0 ? dy : 1));
			y = PANEL_H - 1;
		}

		if (c == c0) {
			*x0 = x;
			*y0 = y;
			c0 = clip_outcode(x, y);
		} else {
			*x1 = x;
			*y1 = y;
			c1 = clip_outcode(x, y);
		}
	}

	/* Rounding can fail to converge — still draw a clamped segment instead of
	 * dropping the line (dropped cube edges + one exploded axis = the stall photo). */
	if (*x0 < 0) {
		*x0 = 0;
	} else if (*x0 >= PANEL_W) {
		*x0 = PANEL_W - 1;
	}
	if (*y0 < 0) {
		*y0 = 0;
	} else if (*y0 >= PANEL_H) {
		*y0 = PANEL_H - 1;
	}
	if (*x1 < 0) {
		*x1 = 0;
	} else if (*x1 >= PANEL_W) {
		*x1 = PANEL_W - 1;
	}
	if (*y1 < 0) {
		*y1 = 0;
	} else if (*y1 >= PANEL_H) {
		*y1 = PANEL_H - 1;
	}
	return true;
}

/*
 * v146 fix — root cause of the recurring "render stall panic" (v143+, EXCCAUSE 63): this ran
 * its Bresenham step math in int16_t. `err` can approach +/-16384 for near-diagonal long lines,
 * and `err * 2` was assigned to an int16_t `e2` — the multiply itself promotes to `int` per C's
 * usual arithmetic conversions, but truncating that back into int16_t silently wraps once the
 * true product exceeds +/-32767. That corrupts the sign of the e2>-ay / e2<ax step decisions,
 * which can desync x0/y0 from ever reaching (x1,y1) — turning `while(1)` into a genuine infinite
 * loop with no watchdog feed inside it. scene_snapshot_build()'s 3D->2D projection can produce
 * endpoint coordinates near INT16_MAX/MIN for certain device orientations (a projected point
 * passing near the camera's near-plane), which is exactly the rare/orientation-dependent trigger
 * that made this so hard to reproduce on demand. Fix: do all step math in int32_t (no overflow
 * possible for any int16_t input pair) and add a hard iteration cap as defense-in-depth in case
 * of any other input pathology — correct Bresenham always terminates in max(ax,ay)+1 steps.
 *
 * v154: clip to the panel first. HUD lives in the top SPI strips; the cube/axes live
 * lower. An unclipped near-plane line could spend the whole frame iterating off-screen (or
 * wrapping) so the cube region looked frozen while the header caption still changed.
 */
void panel_draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
	int32_t ax0 = x0;
	int32_t ay0 = y0;
	int32_t ax1 = x1;
	int32_t ay1 = y1;

	if (!clip_to_panel(&ax0, &ay0, &ax1, &ay1)) {
		return;
	}

	int32_t px = ax0;
	int32_t py = ay0;
	const int32_t dx = ax1 - ax0;
	const int32_t dy = ay1 - ay0;
	const int32_t sx = (dx >= 0) ? 1 : -1;
	const int32_t sy = (dy >= 0) ? 1 : -1;
	const int32_t ax = (dx >= 0) ? dx : -dx;
	const int32_t ay = (dy >= 0) ? dy : -dy;
	int32_t err = ax - ay;
	int32_t guard = (ax > ay ? ax : ay) + 2;

	while (guard-- > 0) {
		panel_fb_put((int16_t)px, (int16_t)py, color);
		if (px == ax1 && py == ay1) {
			break;
		}
		const int32_t e2 = err * 2;

		if (e2 > -ay) {
			err -= ay;
			px += sx;
		}
		if (e2 < ax) {
			err += ax;
			py += sy;
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
