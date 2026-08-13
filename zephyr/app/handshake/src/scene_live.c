/*
 * Live onboard scene — parity with esp32 scene_renderer drawAxis3D + drawProjectionDemo +
 * drawWalkOverlay (Arduino production loop @ 30 Hz).
 */

#include <stdio.h>
#include <time.h>

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>

#include "battery_monitor.h"
#include "board_config.h"
#include "clock_sync.h"
#include "display_panel.h"
#include "imu_pipeline.h"
#include "panel_draw.h"
#include "panel_fb.h"
#include "power_manager.h"
#include "scene_snapshot.h"
#include "scene_zoom.h"

#define COL_TEXT  PANEL_WHITE
#define COL_X     PANEL_RED
#define COL_Y     PANEL_GREEN
#define COL_Z     PANEL_BLUE
#define COL_WALK  PANEL_YELLOW
#define COL_FOOT  PANEL_BLUE

#define SCENE_FONT_AXIS  2   /* upright X/Y/Z — Arduino drawText size 2 */
#define SCENE_FONT_BODY  1

#define HUD_STATS_MS  15000U
#define HUD_DATE_MS   5000U
#define HUD_TIME_MS   10000U
#define HUD_CYCLE_MS  (HUD_STATS_MS + HUD_DATE_MS + HUD_TIME_MS)

static bool g_panel_on;

static void draw_delta_triangle(int16_t x, int16_t y, uint16_t color)
{
	panel_draw_line(x, y + 9, x + 4, y, color);
	panel_draw_line(x + 4, y, x + 8, y + 9, color);
	panel_draw_line(x, y + 9, x + 8, y + 9, color);
	panel_draw_line(x + 1, y + 8, x + 7, y + 8, color);
	panel_draw_line(x + 2, y + 7, x + 6, y + 7, color);
}

static void format_power_caption(char *buf, size_t cap, float distance_m)
{
	const struct battery_state *bat = battery_monitor_state();

	if (bat != NULL && bat->on_dc) {
		if (bat->voltage_v >= BAT_EXTERNAL_V) {
			snprintf(buf, cap, "%.1f m p/s:DC %.2fV", (double)distance_m,
				 (double)bat->voltage_v);
		} else {
			snprintf(buf, cap, "%.1f m p/s:DC ext", (double)distance_m);
		}
	} else if (bat != NULL && bat->valid) {
		snprintf(buf, cap, "%.1f m p/s:BAT %u%%", (double)distance_m, bat->percent);
	} else if (bat != NULL) {
		snprintf(buf, cap, "%.1f m p/s:adc%umV", (double)distance_m, bat->adc_mv);
	} else {
		snprintf(buf, cap, "%.1f m p/s:adc0mV", (double)distance_m);
	}
}

static void format_hud_date(char *buf, size_t cap)
{
	static const char *dow[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
	const int16_t tz_min = clock_sync_tz_offset_min();
	const time_t local_sec = (time_t)(clock_sync_now_ms() / 1000LL + (int64_t)tz_min * 60LL);
	struct tm tm_local;

	if (gmtime_r(&local_sec, &tm_local) == NULL) {
		snprintf(buf, cap, "Clock --");
		return;
	}

	snprintf(buf, cap, "%02d:%02d:%04d, %s", tm_local.tm_mday, tm_local.tm_mon + 1,
		 tm_local.tm_year + 1900, dow[tm_local.tm_wday % 7]);
}

static void format_hud_time(char *buf, size_t cap)
{
	const bool synced = clock_sync_is_synced();
	const char *ntp = synced ? "OK" : "FAIL";

	if (!synced) {
		snprintf(buf, cap, "--:--:-- NTP %s", ntp);
		return;
	}

	const int16_t tz_min = clock_sync_tz_offset_min();
	const time_t local_sec = (time_t)(clock_sync_now_ms() / 1000LL + (int64_t)tz_min * 60LL);
	struct tm tm_local;

	if (gmtime_r(&local_sec, &tm_local) == NULL) {
		snprintf(buf, cap, "--:--:-- NTP %s", ntp);
		return;
	}

	snprintf(buf, cap, "%02d:%02d:%02d NTP %s", tm_local.tm_hour, tm_local.tm_min,
		 tm_local.tm_sec, ntp);
}

static void draw_walk_overlay(float distance_m)
{
	const int16_t hud_x = 10;
	const int16_t hud_y = 16;
	const int16_t hud_h = 20;
	const int16_t hud_w = 168;
	char buf[64];
	uint32_t phase_ms = k_uptime_get_32() % HUD_CYCLE_MS;
	enum { HUD_STATS, HUD_DATE, HUD_TIME } phase = HUD_STATS;

	if (phase_ms >= HUD_STATS_MS && phase_ms < HUD_STATS_MS + HUD_DATE_MS) {
		phase = HUD_DATE;
	} else if (phase_ms >= HUD_STATS_MS + HUD_DATE_MS) {
		phase = HUD_TIME;
	}

	panel_fb_fill_rect(hud_x, hud_y, hud_w, hud_h, PANEL_BLACK);
	draw_delta_triangle(hud_x + 2, hud_y + 2, COL_WALK);

	switch (phase) {
	case HUD_DATE:
		if (clock_sync_is_synced()) {
			format_hud_date(buf, sizeof(buf));
		} else {
			snprintf(buf, sizeof(buf), "Clock --");
		}
		break;
	case HUD_TIME:
		format_hud_time(buf, sizeof(buf));
		break;
	default:
		format_power_caption(buf, sizeof(buf), distance_m);
		break;
	}

	panel_draw_text(hud_x + 14, hud_y + 4, COL_WALK, buf, SCENE_FONT_BODY);
}

void scene_live_init(const struct device *display)
{
	scene_zoom_init();
	panel_fb_begin(PANEL_BLACK);
	panel_fb_flush(display);
	if (power_manager_screen_on()) {
		(void)panel_display_hw_set(display, true);
		g_panel_on = true;
	} else {
		g_panel_on = false;
	}
}

void scene_live_draw(const struct device *display)
{
	struct imu_sample sample;
	struct attitude_estimator att;

	panel_fb_begin(PANEL_BLACK);

	if (imu_pipeline_recovering()) {
		panel_draw_text(8, 40, COL_TEXT, "IMU recovering...", SCENE_FONT_BODY);
		panel_fb_flush(display);
		if (power_manager_screen_on() && !g_panel_on) {
			panel_display_hw_set(display, true);
			g_panel_on = true;
		}
		return;
	}

	if (!imu_pipeline_snapshot(&sample, &att)) {
		panel_draw_text(8, 40, COL_TEXT, "IMU not ready", SCENE_FONT_BODY);
		panel_draw_text(8, 58, COL_TEXT, "retrying I2C...", SCENE_FONT_BODY);
		panel_fb_flush(display);
		if (power_manager_screen_on() && !g_panel_on) {
			panel_display_hw_set(display, true);
			g_panel_on = true;
		}
		return;
	}

	const float walk_m = imu_pipeline_walk_distance_m();
	const struct scene_snapshot snap = scene_snapshot_build(
		PANEL_W, PANEL_H, scene_zoom_current(), &att.state.rotation, &sample, walk_m);

	panel_draw_circle(snap.center_x, snap.center_y, 3, COL_TEXT, true);

	for (int i = 0; i < 3; i++) {
		const uint16_t colors[3] = { COL_X, COL_Y, COL_Z };

		panel_draw_line((int16_t)snap.axes[i].p0.x, (int16_t)snap.axes[i].p0.y,
				(int16_t)snap.axes[i].p1.x, (int16_t)snap.axes[i].p1.y, colors[i]);
	}

	panel_draw_text(4, PANEL_H - 48, COL_X, "X", SCENE_FONT_AXIS);
	panel_draw_text(24, PANEL_H - 48, COL_Y, "Y", SCENE_FONT_AXIS);
	panel_draw_text(44, PANEL_H - 48, COL_Z, "Z", SCENE_FONT_AXIS);

	for (int e = 0; e < 12; e++) {
		static const int edges[12][2] = {
			{ 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 }, { 4, 5 }, { 5, 6 },
			{ 6, 7 }, { 7, 4 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
		};
		const int a = edges[e][0];
		const int b = edges[e][1];

		panel_draw_line((int16_t)snap.corners[a].x, (int16_t)snap.corners[a].y,
				(int16_t)snap.corners[b].x, (int16_t)snap.corners[b].y, COL_TEXT);
	}

	const int16_t footer_y = PANEL_H - 28;
	char buf[48];

	snprintf(buf, sizeof(buf), "2D->3D: %.2f %.2f %.2f", (double)snap.footer_unproject.x,
		 (double)snap.footer_unproject.y, (double)snap.footer_unproject.z);
	panel_draw_text(4, footer_y, COL_FOOT, buf, SCENE_FONT_BODY);

	draw_walk_overlay(walk_m);
	panel_fb_flush(display);

	if (power_manager_screen_on() && !g_panel_on) {
		panel_display_hw_set(display, true);
		g_panel_on = true;
	} else if (!power_manager_screen_on()) {
		g_panel_on = false;
	}
}
