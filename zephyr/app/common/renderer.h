#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

#include "display_panel.h"

/**
 * Unified LCD renderer — PSRAM RGB565 framebuffer; ALL display_write() and
 * display_blanking_*() run only on the claimed render thread.
 *
 * Draw path (render thread only):
 *   renderer_begin() → renderer_put / fill_rect / panel_draw_* → renderer_present()
 *
 * Other threads must not touch the Zephyr display API. Use:
 *   renderer_request_hw(on) / renderer_request_present()
 * and let the render thread drain via renderer_drain() each tick.
 */

/** Bind the Zephyr display device (no SPI). Call once from main. */
void renderer_init(const struct device *display);

/** Call once from the dedicated render thread before any SPI/draw. */
void renderer_claim_thread(void);

/**
 * Drain queued HW/present work. Render thread only — call once per frame
 * before drawing (and after claim at boot).
 */
void renderer_drain(void);

/** Ensure FB exists; does not clear. Render thread only. */
void renderer_ensure(void);

/** Fill FB with clear_color. Render thread only. */
void renderer_begin(uint16_t clear_color);

/** Pixel / rect into the FB (between begin…present). Render thread only. */
void renderer_put(int16_t x, int16_t y, uint16_t color);
void renderer_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/** Flush FB via the sole display_write() strip loop. Render thread only. */
void renderer_present(void);

/** Queue a present for the next drain/present on the render thread. */
void renderer_request_present(void);

/**
 * Queue panel blanking on/off (+ backlight) for the render thread.
 * Safe from any thread. Returns true if applied now or queued.
 */
bool renderer_request_hw(bool on);

/**
 * Apply blanking now if on the render thread; otherwise queue (same as
 * renderer_request_hw). Prefer request_hw from non-render contexts.
 */
bool renderer_hw_set(bool on);

/** Stall-hunt: when false, present() skips SPI (draw math still runs). */
void renderer_set_spi_enabled(bool enabled);

/** True after consecutive SPI failures latched the panel off (clearable). */
bool renderer_spi_tripped(void);

/** Clear soft SPI trip so quiet/radio-epoch end can redraw again. */
void renderer_spi_clear_trip(void);

uint32_t renderer_last_present_ms(void);
bool renderer_busy(void);

/**
 * Exclusive panel-SPI vs IMU-I2C gate (MSPI/cache hazard on ESP32-S3).
 * Render holds across flush; IMU try-locks and skips if SPI owns the bus.
 */
void renderer_ext_bus_lock(void);
void renderer_ext_bus_unlock(void);
bool renderer_ext_bus_try(void);

/**
 * Diagnostic for render-stall panic — which strip display_write is in, if any.
 * Returns written length, or 0 if idle.
 */
int renderer_stall_info(char *buf, size_t buf_len);

/** True once the render thread has claimed ownership. */
bool renderer_ready(void);

/** True if the calling thread is the claimed render owner. */
bool renderer_on_owner_thread(void);
