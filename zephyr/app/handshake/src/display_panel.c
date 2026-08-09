#include <string.h>

#include <zephyr/sys/util.h>

#include "display_panel.h"

static void fill_rect_rows(const struct device *display, uint16_t x, uint16_t y,
			   uint16_t w, uint16_t h, uint16_t color)
{
	static uint16_t row[PANEL_W];
	struct display_buffer_descriptor desc = {
		.width = w,
		.height = 1,
		.pitch = w,
		.buf_size = w * sizeof(uint16_t),
	};

	for (uint16_t col = 0; col < w; col++) {
		row[col] = color;
	}

	for (uint16_t row_y = 0; row_y < h; row_y++) {
		display_write(display, x, y + row_y, &desc, row);
	}
}

void panel_fill_rect(const struct device *display, uint16_t x, uint16_t y, uint16_t w,
		     uint16_t h, uint16_t color)
{
	if (w > PANEL_W) {
		return;
	}

	fill_rect_rows(display, x, y, w, h, color);
}

void panel_clear_screen(const struct device *display, uint16_t color)
{
	for (uint16_t y = 0; y < PANEL_H; y += PANEL_STRIP) {
		uint16_t h = PANEL_STRIP;

		if (y + h > PANEL_H) {
			h = PANEL_H - y;
		}

		panel_fill_rect(display, 0, y, PANEL_W, h, color);
	}
}

void panel_draw_corners(const struct device *display)
{
	const uint16_t w = 48;
	const uint16_t h = 48;
	const uint16_t colors[4] = {PANEL_RED, PANEL_GREEN, PANEL_BLUE, PANEL_GREY};
	const uint16_t xs[4] = {0, PANEL_W - w, PANEL_W - w, 0};
	const uint16_t ys[4] = {0, 0, PANEL_H - h, PANEL_H - h};

	for (int i = 0; i < 4; i++) {
		panel_fill_rect(display, xs[i], ys[i], w, h, colors[i]);
	}

	display_blanking_off(display);
}

void panel_boot_bar(const struct device *display, uint16_t color)
{
	const uint16_t y0 = PANEL_H / 2 - PANEL_BAR_H / 2;

	panel_fill_rect(display, 0, y0, PANEL_W, PANEL_BAR_H, PANEL_BLACK);
	panel_fill_rect(display, 0, y0, PANEL_W, PANEL_BAR_H, color);
}
