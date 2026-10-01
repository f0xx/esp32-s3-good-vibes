#include <stdio.h>
#include <string.h>

#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "renderer.h"

#include "app_func_trace.h"
#include "display_panel.h"
#include "panel_backlight.h"
#include "stack_ra_check.h"
#include "stall_watchdog.h"

#if IS_ENABLED(CONFIG_ESP_SPIRAM)
#include <soc/soc_memory_layout.h>
#endif

LOG_MODULE_REGISTER(renderer, LOG_LEVEL_INF);

/*
 * Strip height sized so one display_write() fits under SPI_DMA_MAX_BUFFER_SIZE
 * (4092) when the bounce buffer is internal DRAM: 11 * 172 * 2 = 3784.
 *
 * Single PSRAM framebuffer; all Zephyr display_* SPI runs on the claimed
 * render thread only. Other contexts enqueue via renderer_request_*.
 */
#define FB_FLUSH_ROWS 11
#define FB_PIXELS     ((size_t)PANEL_W * (size_t)PANEL_H)
#define FB_BYTES      (FB_PIXELS * sizeof(uint16_t))
#define STRIP_WARN_MS 250U

/* Desk prj_crash turns APP_STALL_WATCHDOG off; feed calls become no-ops. */
#define DISABLE_STALL_WATCHDOG (!IS_ENABLED(CONFIG_APP_STALL_WATCHDOG))

#if IS_ENABLED(CONFIG_ESP_SPIRAM)
static uint16_t fb_psram[FB_PIXELS] __attribute__((section(".ext_ram.bss"))) __aligned(32);
#endif

static uint16_t *fb;
static uint16_t strip_ram[PANEL_W * (FB_FLUSH_ROWS + 1)] __aligned(32);

static const struct device *g_display;
static volatile bool fb_ready;
static volatile bool g_display_busy;
static volatile bool g_spi_enabled = true;
/* After SPI -ETIMEDOUT, skip flushes briefly so MSPI can recover under MT200. */
static uint32_t g_spi_cooldown_until_ms;
/*
 * Soft trip: consecutive display_write failures. Cleared on success or
 * renderer_spi_clear_trip() (quiet / radio epoch end). Permanent latch was
 * freezing the cube for the rest of the boot while BLE/phone kept moving (v307).
 */
#define SPI_FAIL_TRIP_STREAK 3U
static volatile uint8_t g_spi_fail_streak;
static volatile bool g_spi_tripped;
static volatile bool g_in_frame;

static atomic_t g_present_requested;
static atomic_t g_hw_pending;
static atomic_t g_hw_want; /* 0=off, 1=on */

static volatile uint32_t g_last_present_ms;
static volatile uint32_t g_last_copy_ms;
static volatile uint32_t g_last_write_ms;

static volatile int16_t g_flush_cur_y = -1;
static volatile uint32_t g_flush_cur_start_ms;
static volatile int g_flush_last_ret;
static volatile uint32_t g_flush_last_elapsed_ms;

static struct display_buffer_descriptor desc = {
	.width = PANEL_W,
	.height = FB_FLUSH_ROWS,
	.pitch = PANEL_W,
};

static k_tid_t g_owner;
static struct k_mutex g_ext_bus;
static bool g_ext_bus_ready;

static bool on_owner_thread(void)
{
	return g_owner != NULL && k_current_get() == g_owner;
}

void renderer_ext_bus_lock(void)
{
	if (g_ext_bus_ready) {
		(void)k_mutex_lock(&g_ext_bus, K_FOREVER);
	}
}

void renderer_ext_bus_unlock(void)
{
	if (g_ext_bus_ready) {
		k_mutex_unlock(&g_ext_bus);
	}
}

bool renderer_ext_bus_try(void)
{
	if (!g_ext_bus_ready) {
		return true;
	}
	return k_mutex_lock(&g_ext_bus, K_NO_WAIT) == 0;
}

static void assert_owner(void)
{
	if (g_owner == NULL) {
		return;
	}
	__ASSERT(k_current_get() == g_owner, "renderer SPI/draw must run on render thread");
}

static int ensure_fb(void)
{
	if (fb_ready) {
		return 0;
	}

	if (fb == NULL) {
#if IS_ENABLED(CONFIG_ESP_SPIRAM)
		fb = fb_psram;
#else
		fb = k_malloc(FB_BYTES);
#endif
	}
	if (fb == NULL) {
		LOG_ERR("framebuffer alloc failed (%u KiB)", (unsigned)(FB_BYTES / 1024U));
		return -ENOMEM;
	}

#if IS_ENABLED(CONFIG_ESP_SPIRAM)
	if (!esp_ptr_external_ram(fb)) {
		LOG_ERR("framebuffer %p not in PSRAM — check esp_init_psram / .ext_ram.bss",
			(void *)fb);
		return -EIO;
	}
	LOG_INF("framebuffer ready (%u KiB via renderer, .ext_ram.bss @%p)",
		(unsigned)(FB_BYTES / 1024U), (void *)fb);
#else
	LOG_INF("framebuffer ready (%u KiB via renderer, internal)",
		(unsigned)(FB_BYTES / 1024U));
#endif
	fb_ready = true;
	return 0;
}

static inline bool in_bounds(int16_t x, int16_t y)
{
	return x >= 0 && y >= 0 && x < PANEL_W && y < PANEL_H;
}

static void clear_buffer(uint16_t clear_color)
{
	if (clear_color == PANEL_BLACK) {
		uint8_t *p = (uint8_t *)fb;

		/*
		 * Chunked PSRAM clear — feed soft-WDT every 32 KiB like old panel_fb.
		 * Do NOT k_yield() here: yielding mid-MSPI clear under BLE lets the BT
		 * stack run and has wedged this S3 (illegal insn / silent DRAW hang
		 * while WDT stays fed).
		 */
		for (size_t off = 0; off < FB_BYTES; off += 8192U) {
			const size_t n = MIN(8192U, FB_BYTES - off);

			memset(p + off, 0, n);
#if (!DISABLE_STALL_WATCHDOG)
			if ((off & 0x7fffU) == 0U) {
				stall_watchdog_feed_render();
			}
#endif
		}
		return;
	}

	const uint32_t pair = ((uint32_t)clear_color << 16) | clear_color;
	uint32_t *dst = (uint32_t *)fb;

	for (size_t i = 0; i < FB_PIXELS / 2U; i++) {
		dst[i] = pair;
	}
}

static void fill_rect_buf(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
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

	if (color == PANEL_BLACK && x == 0U && w == PANEL_W) {
		memset(&fb[(size_t)y * PANEL_W], 0, (size_t)h * PANEL_W * sizeof(uint16_t));
		return;
	}

	for (uint16_t row = 0; row < h; row++) {
		uint16_t *dst = &fb[(size_t)(y + row) * PANEL_W + x];
		uint16_t col = 0;

		if ((w > 0U) && (((uintptr_t)dst) & 2U)) {
			dst[0] = color;
			col = 1U;
		}
		{
			const uint16_t remain = (uint16_t)(w - col);
			const uint16_t pairs = remain / 2U;
			uint32_t *d32 = (uint32_t *)&dst[col];
			const uint32_t pair = ((uint32_t)color << 16) | color;

			for (uint16_t i = 0; i < pairs; i++) {
				d32[i] = pair;
			}
			col = (uint16_t)(col + pairs * 2U);
		}
		if (col < w) {
			dst[col] = color;
		}
	}
}

/** Sole display_write() call site for Good Vibes Zephyr apps. Owner thread only. */
static void flush_fb(const struct device *display)
{
	STACK_RA_CHECK_SETUP;
	uint32_t copy_ms = 0U;
	uint32_t write_ms = 0U;

	renderer_ext_bus_lock();

	/* No LOG_* on this thread. Immediate logging (prj.conf) takes the log
	 * slab from the render thread and has faulted inside
	 * z_log_msg_runtime_create (v314 pending: thread "render", reason fatal,
	 * then SW reboot). That reboot is what replays scene_zoom from the
	 * default pose. Breadcrumbs stay in g_last_*_ms. */

	const uint32_t t0 = k_uptime_get_32();

	for (uint16_t y = 0; y < PANEL_H; y += FB_FLUSH_ROWS) {
		uint16_t rows = FB_FLUSH_ROWS;

		if (y + rows > PANEL_H) {
			rows = PANEL_H - y;
		}

		desc.height = rows;
		desc.buf_size = (size_t)desc.pitch * rows * sizeof(uint16_t);

		const uint32_t tc0 = k_uptime_get_32();
		memcpy(strip_ram, &fb[(size_t)y * desc.pitch], desc.buf_size);
		copy_ms += k_uptime_get_32() - tc0;

		g_flush_cur_y = (int16_t)y;
		g_flush_cur_start_ms = k_uptime_get_32();

		const uint32_t tw0 = g_flush_cur_start_ms;
		const int ret = display_write(display, 0, y, &desc, strip_ram);
		const uint32_t elapsed = k_uptime_get_32() - tw0;

		write_ms += elapsed;
		g_flush_last_ret = ret;
		g_flush_last_elapsed_ms = elapsed;
		g_flush_cur_y = -1;

		STACK_RA_CHECK();

		if (ret != 0) {
			/* No LOG_* here — fatal/logging after MSPI smash DoubleException'd
			 * the desk board (EXCCAUSE StoreProhibited in z_log_msg). */
			g_flush_last_ret = ret;
			g_spi_cooldown_until_ms = k_uptime_get_32() + 5000U;
			if (g_spi_fail_streak < 255U) {
				g_spi_fail_streak++;
			}
			g_spi_enabled = false;
			/* Soft trip after streak — recoverable via renderer_spi_clear_trip. */
			if (g_spi_fail_streak >= SPI_FAIL_TRIP_STREAK) {
				g_spi_tripped = true;
			}
			break;
		}
		g_spi_fail_streak = 0U;
		if (elapsed > STRIP_WARN_MS) {
			g_flush_last_elapsed_ms = elapsed;
		}
#if (!DISABLE_STALL_WATCHDOG)
		stall_watchdog_feed_render();
#endif
	}

	g_last_copy_ms = copy_ms;
	g_last_write_ms = write_ms;
	g_last_present_ms = k_uptime_get_32() - t0;

	STACK_RA_CHECK();
	renderer_ext_bus_unlock();
}

static bool hw_apply_now(bool on)
{
	STACK_RA_CHECK_SETUP;
	assert_owner();
	if (g_display == NULL) {
		return false;
	}

	if (!on) {
		panel_backlight_set_on(false);
	}
#if (!DISABLE_STALL_WATCHDOG)
	stall_watchdog_feed_render();
#endif
	renderer_ext_bus_lock();
	g_display_busy = true;
	if (on) {
		(void)display_blanking_off(g_display);
	} else {
		(void)display_blanking_on(g_display);
	}
	g_display_busy = false;
	renderer_ext_bus_unlock();
	STACK_RA_CHECK();

	if (on) {
		panel_backlight_set_on(true);
	}
	return true;
}

void renderer_init(const struct device *display)
{
	g_display = display;
	k_mutex_init(&g_ext_bus);
	g_ext_bus_ready = true;
	(void)ensure_fb();
	LOG_INF("renderer init display=%p (SPI deferred to render thread)", (void *)display);
}

void renderer_claim_thread(void)
{
	g_owner = k_current_get();
	LOG_INF("renderer owner tid=%p", (void *)g_owner);
}

bool renderer_ready(void)
{
	return g_owner != NULL;
}

bool renderer_on_owner_thread(void)
{
	return on_owner_thread();
}

void renderer_drain(void)
{
	STACK_RA_CHECK_SETUP;
	assert_owner();

	if (atomic_cas(&g_hw_pending, 1, 0)) {
		const bool on = atomic_get(&g_hw_want) != 0;

		(void)hw_apply_now(on);
		STACK_RA_CHECK();
	}

	if (atomic_cas(&g_present_requested, 1, 0)) {
		if (fb != NULL && g_display != NULL && g_spi_enabled && !g_in_frame &&
		    (int32_t)(k_uptime_get_32() - g_spi_cooldown_until_ms) >= 0) {
			/*
			 * Hold busy across the whole flush so soft-WDT and power
			 * paths treat mid-SPI the same as a live frame.
			 */
			g_display_busy = true;
			flush_fb(g_display);
			g_display_busy = false;
			STACK_RA_CHECK();
		}
	}
}

void renderer_ensure(void)
{
	assert_owner();
	(void)ensure_fb();
}

void renderer_begin(uint16_t clear_color)
{
	assert_owner();
	if (ensure_fb() != 0) {
		g_in_frame = false;
		g_display_busy = false;
		return;
	}

	/*
	 * Mark busy for the whole begin→present window (PSRAM clear + draw +
	 * SPI), not just flush. Mid-frame CPU DFS / sleep while BT is active
	 * has wedged MSPI on this board.
	 */
	g_display_busy = true;
	g_in_frame = true;
	clear_buffer(clear_color);
}

void renderer_put(int16_t x, int16_t y, uint16_t color)
{
	assert_owner();
	if (!g_in_frame || fb == NULL || !in_bounds(x, y)) {
		return;
	}

	fb[(size_t)y * PANEL_W + (size_t)x] = color;
}

void renderer_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
	assert_owner();
	if (!g_in_frame || fb == NULL) {
		return;
	}
	fill_rect_buf(x, y, w, h, color);
}

void renderer_set_spi_enabled(bool enabled)
{
	if (g_spi_tripped) {
		g_spi_enabled = false;
		return;
	}
	g_spi_enabled = enabled;
}

bool renderer_spi_tripped(void)
{
	return g_spi_tripped;
}

void renderer_spi_clear_trip(void)
{
	g_spi_tripped = false;
	g_spi_fail_streak = 0U;
	g_spi_cooldown_until_ms = 0U;
	/* Re-enable only if caller also wants SPI; render loop sets enabled. */
}

void renderer_request_present(void)
{
	atomic_set(&g_present_requested, 1);
}

bool renderer_request_hw(bool on)
{
	atomic_set(&g_hw_want, on ? 1 : 0);
	atomic_set(&g_hw_pending, 1);

	/* Fast path: already on render thread — apply immediately. */
	if (on_owner_thread()) {
		if (atomic_cas(&g_hw_pending, 1, 0)) {
			return hw_apply_now(on);
		}
	}
	return true;
}

void renderer_present(void)
{
	assert_owner();

	const bool had_frame = g_in_frame;

	g_in_frame = false;

	if (fb == NULL || g_display == NULL) {
		g_display_busy = false;
		return;
	}

	if (!had_frame && !atomic_cas(&g_present_requested, 1, 0)) {
		g_display_busy = false;
		return;
	}
	(void)atomic_set(&g_present_requested, 0);

	if (!g_spi_enabled) {
		g_last_present_ms = 0U;
		g_last_copy_ms = 0U;
		g_last_write_ms = 0U;
		g_display_busy = false;
		return;
	}

	if ((int32_t)(k_uptime_get_32() - g_spi_cooldown_until_ms) < 0) {
		g_last_present_ms = 0U;
		g_last_copy_ms = 0U;
		g_last_write_ms = 0U;
		g_display_busy = false;
		return;
	}

	/* busy already true from begin(); keep through flush */
	flush_fb(g_display);
	g_display_busy = false;
}

bool renderer_hw_set(bool on)
{
	APP_ENTER();
	const bool ok = renderer_request_hw(on);
	APP_LEAVE();
	return ok;
}

int renderer_stall_info(char *buf, size_t buf_len)
{
	const int16_t y = g_flush_cur_y;

	if (buf == NULL || buf_len == 0U) {
		return 0;
	}
	if (y < 0) {
		const int n = snprintf(buf, buf_len,
				       "idle (last_ret=%d last=%ums copy=%u write=%u total=%u)",
				       g_flush_last_ret, g_flush_last_elapsed_ms, g_last_copy_ms,
				       g_last_write_ms, g_last_present_ms);
		return (n > 0 && (size_t)n < buf_len) ? n : 0;
	}

	const uint32_t since = k_uptime_get_32() - g_flush_cur_start_ms;
	const int n = snprintf(buf, buf_len, "display_write(y=%d) running for %ums so far", y,
			       since);

	return (n > 0 && (size_t)n < buf_len) ? n : 0;
}

bool renderer_busy(void)
{
	return g_display_busy;
}

uint32_t renderer_last_present_ms(void)
{
	return g_last_present_ms;
}
