#include <string.h>

#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "panel_fb.h"

#if IS_ENABLED(CONFIG_ESP_SPIRAM)
#include <esp_heap_caps.h>
#endif

#include "display_panel.h"
#include "panel_backlight.h"
#include "stall_watchdog.h"

LOG_MODULE_REGISTER(panel_fb, LOG_LEVEL_INF);

static K_MUTEX_DEFINE(g_display_mtx);

#define FB_FLUSH_ROWS 80
#define FB_PIXELS     ((size_t)PANEL_W * (size_t)PANEL_H)
#define FB_BYTES      (FB_PIXELS * sizeof(uint16_t))
#define DISPLAY_MTX_WAIT_MS 2000

static uint16_t *fb;
static bool fb_ready;
static bool fb_failed;
static bool fb_logged;
static volatile bool g_display_busy;
static uint32_t g_last_flush_ms;

static int ensure_fb(void)
{
	if (fb_ready) {
		return 0;
	}
	if (fb_failed) {
		return -ENOMEM;
	}

#if IS_ENABLED(CONFIG_ESP_SPIRAM)
	fb = heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
	if (fb == NULL) {
		fb = k_malloc(FB_BYTES);
	}
	if (fb == NULL) {
		fb_failed = true;
		LOG_ERR("framebuffer alloc failed (%u KiB)", (unsigned)(FB_BYTES / 1024U));
		return -ENOMEM;
	}

	fb_ready = true;
	return 0;
}

static inline bool in_bounds(int16_t x, int16_t y)
{
	return x >= 0 && y >= 0 && x < PANEL_W && y < PANEL_H;
}

void panel_fb_begin(uint16_t clear_color)
{
	if (ensure_fb() != 0) {
		return;
	}

	if (clear_color == PANEL_BLACK) {
		memset(fb, 0, FB_BYTES);
		return;
	}

	for (size_t i = 0; i < FB_PIXELS; i++) {
		fb[i] = clear_color;
	}
}

void panel_fb_put(int16_t x, int16_t y, uint16_t color)
{
	if (fb == NULL || !in_bounds(x, y)) {
		return;
	}

	fb[(size_t)y * PANEL_W + (size_t)x] = color;
}

void panel_fb_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
	if (fb == NULL || x >= PANEL_W || y >= PANEL_H || w == 0 || h == 0) {
		return;
	}

	if (x + w > PANEL_W) {
		w = PANEL_W - x;
	}
	if (y + h > PANEL_H) {
		h = PANEL_H - y;
	}

	for (uint16_t row = 0; row < h; row++) {
		uint16_t *dst = &fb[(size_t)(y + row) * PANEL_W + x];

		for (uint16_t col = 0; col < w; col++) {
			dst[col] = color;
		}
	}
}

void panel_fb_flush(const struct device *display)
{
	struct display_buffer_descriptor desc = {
		.width = PANEL_W,
		.height = FB_FLUSH_ROWS,
		.pitch = PANEL_W,
	};

	if (fb == NULL || display == NULL) {
		return;
	}

	if (!fb_logged) {
		LOG_INF("framebuffer %ux%u (%u KiB), flush %u-row strips",
			PANEL_W, PANEL_H, (unsigned)(FB_BYTES / 1024U), FB_FLUSH_ROWS);
		fb_logged = true;
	}

	g_display_busy = true;

	if (k_mutex_lock(&g_display_mtx, K_MSEC(DISPLAY_MTX_WAIT_MS)) != 0) {
		LOG_WRN("display flush skipped — mutex busy");
		g_display_busy = false;
		return;
	}

	const uint32_t t0 = k_uptime_get_32();

	for (uint16_t y = 0; y < PANEL_H; y += FB_FLUSH_ROWS) {
		uint16_t rows = FB_FLUSH_ROWS;

		if (y + rows > PANEL_H) {
			rows = PANEL_H - y;
		}

		desc.height = rows;
		desc.buf_size = (size_t)PANEL_W * rows * sizeof(uint16_t);
		display_write(display, 0, y, &desc, &fb[(size_t)y * PANEL_W]);
		stall_watchdog_feed_render();
	}

	g_last_flush_ms = k_uptime_get_32() - t0;
	k_mutex_unlock(&g_display_mtx);
	g_display_busy = false;
}

bool panel_display_busy(void)
{
	return g_display_busy;
}

bool panel_display_hw_set(const struct device *display, bool on)
{
	if (display == NULL) {
		return false;
	}

	g_display_busy = true;

	if (!on) {
		panel_backlight_set_on(false);
	}

	stall_watchdog_feed_render();

	if (k_mutex_lock(&g_display_mtx, K_MSEC(DISPLAY_MTX_WAIT_MS)) != 0) {
		LOG_WRN("display blanking skipped — mutex busy (on=%d)", on ? 1 : 0);
		g_display_busy = false;
		return false;
	}

	if (on) {
		(void)display_blanking_off(display);
	} else {
		(void)display_blanking_on(display);
	}
	k_mutex_unlock(&g_display_mtx);

	stall_watchdog_feed_render();

	if (on) {
		panel_backlight_set_on(true);
	}

	g_display_busy = false;
	return true;
}

uint32_t panel_fb_last_flush_ms(void)
{
	return g_last_flush_ms;
}
