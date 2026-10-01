#include "host_fb.h"

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "host_debug.h"

LOG_MODULE_REGISTER(host_fb, LOG_LEVEL_INF);

static uint16_t g_pixels[HOST_FB_W * HOST_FB_H];
static K_MUTEX_DEFINE(g_lock);
static bool g_sdl_wanted = true;
static bool g_sdl_active;

#if defined(HOST_HAS_SDL2)
#include <SDL.h>

static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture *g_tex;
#endif

uint16_t *host_fb_pixels(void)
{
	return g_pixels;
}

void host_fb_lock(void)
{
	k_mutex_lock(&g_lock, K_FOREVER);
}

void host_fb_unlock(void)
{
	k_mutex_unlock(&g_lock);
}

void host_fb_sdl_set_wanted(bool want)
{
	g_sdl_wanted = want;
}

bool host_fb_sdl_available(void)
{
#if defined(HOST_HAS_SDL2)
	return true;
#else
	return false;
#endif
}

bool host_fb_sdl_enabled(void)
{
	return g_sdl_active;
}

#if defined(HOST_HAS_SDL2)
static bool env_headless(void)
{
	const char *no = getenv("HOST_NO_SDL");
	const char *disp = getenv("DISPLAY");
	const char *way = getenv("WAYLAND_DISPLAY");

	if (no && no[0] && strcmp(no, "0") != 0) {
		return true;
	}
	/* Pipeline / ssh without X11 or Wayland → treat as headless. */
	if ((disp == NULL || disp[0] == '\0') && (way == NULL || way[0] == '\0')) {
		return true;
	}
	return false;
}
#endif

int host_fb_sdl_init(void)
{
	g_sdl_active = false;
#if !defined(HOST_HAS_SDL2)
	LOG_INF("SDL2 not compiled in — framebuffer only");
	return 0;
#else
	if (!g_sdl_wanted) {
		LOG_INF("SDL disabled by --no-sdl");
		return 0;
	}
	if (env_headless()) {
		LOG_INF("SDL auto-skip (headless / HOST_NO_SDL) — fb-only");
		return 0;
	}
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		LOG_WRN("SDL_Init failed: %s — continuing without window", SDL_GetError());
		return 0;
	}
	g_win = SDL_CreateWindow("handshake_host", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
				 HOST_FB_W * 2, HOST_FB_H * 2, SDL_WINDOW_SHOWN);
	if (!g_win) {
		LOG_WRN("SDL_CreateWindow failed: %s", SDL_GetError());
		SDL_Quit();
		return 0;
	}
	g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!g_ren) {
		g_ren = SDL_CreateRenderer(g_win, -1, 0);
	}
	if (!g_ren) {
		LOG_WRN("SDL_CreateRenderer failed: %s", SDL_GetError());
		SDL_DestroyWindow(g_win);
		g_win = NULL;
		SDL_Quit();
		return 0;
	}
	g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
				  HOST_FB_W, HOST_FB_H);
	if (!g_tex) {
		LOG_WRN("SDL_CreateTexture failed: %s", SDL_GetError());
		SDL_DestroyRenderer(g_ren);
		SDL_DestroyWindow(g_win);
		g_ren = NULL;
		g_win = NULL;
		SDL_Quit();
		return 0;
	}
	g_sdl_active = true;
	LOG_INF("SDL window %dx%d (2x scale) ready", HOST_FB_W, HOST_FB_H);
	return 0;
#endif
}

void host_fb_sdl_present(void)
{
#if defined(HOST_HAS_SDL2)
	SDL_Event ev;

	if (!g_sdl_active) {
		return;
	}
	while (SDL_PollEvent(&ev)) {
		if (ev.type == SDL_QUIT) {
			LOG_INF("SDL quit event");
			g_sdl_active = false;
			return;
		}
	}
	host_fb_lock();
	SDL_UpdateTexture(g_tex, NULL, g_pixels, HOST_FB_W * (int)sizeof(uint16_t));
	host_fb_unlock();
	SDL_RenderClear(g_ren);
	SDL_RenderCopy(g_ren, g_tex, NULL, NULL);
	SDL_RenderPresent(g_ren);
#else
	(void)0;
#endif
}

void host_fb_sdl_shutdown(void)
{
#if defined(HOST_HAS_SDL2)
	if (g_tex) {
		SDL_DestroyTexture(g_tex);
		g_tex = NULL;
	}
	if (g_ren) {
		SDL_DestroyRenderer(g_ren);
		g_ren = NULL;
	}
	if (g_win) {
		SDL_DestroyWindow(g_win);
		g_win = NULL;
	}
	if (g_sdl_active || SDL_WasInit(SDL_INIT_VIDEO)) {
		SDL_Quit();
	}
	g_sdl_active = false;
	LOG_INF("SDL shutdown");
#endif
}
