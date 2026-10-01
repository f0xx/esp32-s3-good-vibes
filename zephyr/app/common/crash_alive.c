#include "crash_alive.h"

#include <stddef.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>

#define CRASH_ALIVE_MAGIC 0x414C5646U /* ALVF — v2 breadcrumb layout */

struct crash_alive_slot {
	uint32_t magic;
	uint32_t uptime_ms;
	uint8_t dirty; /* 1 = running; 0 = clean shutdown */
	uint8_t render_stage;
	uint8_t pad[2];
	uint32_t timer_ticks;
	char main_step[12];
	uint32_t crc32;
} __packed;

static struct crash_alive_slot g_alive __attribute__((section(".rtc_noinit.crash_alive")));
static struct k_timer g_tick_timer;
static volatile uint32_t g_timer_ticks;
static bool g_timer_started;

static uint32_t alive_crc(const struct crash_alive_slot *s)
{
	return crc32_ieee((const uint8_t *)s, offsetof(struct crash_alive_slot, crc32));
}

static bool alive_valid(void)
{
	return g_alive.magic == CRASH_ALIVE_MAGIC && g_alive.crc32 == alive_crc(&g_alive);
}

static void tick_timer_fn(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	g_timer_ticks++;
	/* Light RTC stamp — no USB. Survives if main wedges but timers still run. */
	g_alive.magic = CRASH_ALIVE_MAGIC;
	g_alive.timer_ticks = g_timer_ticks;
	g_alive.dirty = 1U;
	g_alive.crc32 = alive_crc(&g_alive);
}

void crash_alive_init(void)
{
	if (g_timer_started) {
		return;
	}
	g_timer_ticks = 0U;
	k_timer_init(&g_tick_timer, tick_timer_fn, NULL);
	k_timer_start(&g_tick_timer, K_SECONDS(1), K_SECONDS(1));
	g_timer_started = true;
}

void crash_alive_mark_running(uint32_t uptime_ms, const char *main_step, uint8_t render_stage)
{
	g_alive.magic = CRASH_ALIVE_MAGIC;
	g_alive.uptime_ms = uptime_ms;
	g_alive.dirty = 1U;
	g_alive.render_stage = render_stage;
	g_alive.timer_ticks = g_timer_ticks;
	memset(g_alive.main_step, 0, sizeof(g_alive.main_step));
	if (main_step != NULL) {
		strncpy(g_alive.main_step, main_step, sizeof(g_alive.main_step) - 1U);
	}
	g_alive.crc32 = alive_crc(&g_alive);
}

void crash_alive_mark_clean_shutdown(void)
{
	if (!alive_valid()) {
		memset(&g_alive, 0, sizeof(g_alive));
		return;
	}
	g_alive.dirty = 0U;
	g_alive.crc32 = alive_crc(&g_alive);
}

bool crash_alive_dirty(void)
{
	return alive_valid() && g_alive.dirty != 0U;
}

uint32_t crash_alive_last_uptime_ms(void)
{
	return alive_valid() ? g_alive.uptime_ms : 0U;
}

uint32_t crash_alive_last_timer_ticks(void)
{
	return alive_valid() ? g_alive.timer_ticks : 0U;
}

void crash_alive_last_step(char *out, size_t out_len)
{
	if (out == NULL || out_len == 0U) {
		return;
	}
	out[0] = '\0';
	if (!alive_valid()) {
		return;
	}
	strncpy(out, g_alive.main_step, out_len - 1U);
	out[out_len - 1U] = '\0';
}

uint8_t crash_alive_last_render_stage(void)
{
	return alive_valid() ? g_alive.render_stage : 0U;
}

void crash_alive_consume(void)
{
	memset(&g_alive, 0, sizeof(g_alive));
}
