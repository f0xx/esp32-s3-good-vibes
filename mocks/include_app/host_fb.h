#pragma once

#include <stdbool.h>
#include <stdint.h>

enum { HOST_FB_W = 172, HOST_FB_H = 320 };

/** RGB565 framebuffer shared with optional SDL window. */
uint16_t *host_fb_pixels(void);
void host_fb_lock(void);
void host_fb_unlock(void);

/**
 * Optional SDL path. Auto-disabled when:
 *  - built without SDL2
 *  - DISPLAY unset / WAYLAND_DISPLAY unset (headless)
 *  - HOST_NO_SDL=1 or --no-sdl
 * Never fails the sim if SDL init fails.
 */
bool host_fb_sdl_available(void);
bool host_fb_sdl_enabled(void);
void host_fb_sdl_set_wanted(bool want);
int host_fb_sdl_init(void);   /* 0 ok / disabled, <0 hard fail (unused — we soft-fail) */
void host_fb_sdl_present(void);
void host_fb_sdl_shutdown(void);
