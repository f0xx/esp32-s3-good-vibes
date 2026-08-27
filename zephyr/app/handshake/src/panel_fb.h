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

/**
 * Diagnostic for the recurring "render stall panic" (see main.c's render_stage_mark) — narrows a
 * stall inside panel_fb_flush() down to which row-strip's display_write() (SPI transceive, see
 * zephyr/drivers/spi/spi_context.h) hasn't returned yet, and how long it's been running. Each
 * strip is copied to internal SRAM first (PSRAM FB is not DMA-capable) and should bound to
 * roughly transfer-time + CONFIG_SPI_COMPLETION_TIMEOUT_TOLERANCE (~200ms default) per Zephyr's
 * spi_context_wait_for_completion() — so a multi-second reading here points at the *other*
 * unbounded wait in that call chain, spi_context_lock()'s bus-mutex k_sem_take(..., K_FOREVER),
 * rather than the transfer itself.
 * Returns written length, or 0 if not currently flushing.
 */
int panel_fb_flush_stall_info(char *buf, size_t buf_len);

/** Thread-safe display blanking; backlight is driven outside the flush mutex. */
bool panel_display_hw_set(const struct device *display, bool on);
