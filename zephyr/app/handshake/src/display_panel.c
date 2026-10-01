#include <zephyr/sys/util.h>

#include "display_panel.h"
#include "renderer.h"

/*
 * Legacy helpers — draw via renderer. Off the owner thread only queue HW/present
 * (never begin/fill/present: that asserts and/or races SPI). Prefer scene_live
 * on the render thread.
 */

void panel_fill_rect(const struct device *display, uint16_t x, uint16_t y, uint16_t w,
		     uint16_t h, uint16_t color)
{
	ARG_UNUSED(display);

	if (!renderer_ready() || !renderer_on_owner_thread()) {
		renderer_request_present();
		return;
	}
	renderer_ensure();
	renderer_begin(PANEL_BLACK);
	renderer_fill_rect(x, y, w, h, color);
	renderer_present();
}

void panel_clear_screen(const struct device *display, uint16_t color)
{
	ARG_UNUSED(display);

	if (!renderer_ready() || !renderer_on_owner_thread()) {
		renderer_request_present();
		return;
	}
	renderer_begin(color);
	renderer_present();
}

void panel_draw_corners(const struct device *display)
{
	const uint16_t w = 48;
	const uint16_t h = 48;
	const uint16_t colors[4] = {PANEL_RED, PANEL_GREEN, PANEL_BLUE, PANEL_GREY};
	const uint16_t xs[4] = {0, PANEL_W - w, PANEL_W - w, 0};
	const uint16_t ys[4] = {0, 0, PANEL_H - h, PANEL_H - h};

	ARG_UNUSED(display);

	if (!renderer_ready() || !renderer_on_owner_thread()) {
		(void)renderer_request_hw(true);
		renderer_request_present();
		return;
	}
	renderer_begin(PANEL_BLACK);
	for (int i = 0; i < 4; i++) {
		renderer_fill_rect(xs[i], ys[i], w, h, colors[i]);
	}
	renderer_present();
	(void)renderer_request_hw(true);
}

void panel_boot_bar(const struct device *display, uint16_t color)
{
	const uint16_t y0 = PANEL_H / 2 - PANEL_BAR_H / 2;

	ARG_UNUSED(display);

	if (!renderer_ready() || !renderer_on_owner_thread()) {
		renderer_request_present();
		return;
	}
	renderer_begin(PANEL_BLACK);
	renderer_fill_rect(0, y0, PANEL_W, PANEL_BAR_H, color);
	renderer_present();
}
