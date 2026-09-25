#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "attitude.h"
#include "ble_host.h"
#include "host_debug.h"
#include "host_fb.h"
#include "host_latency.h"
#include "host_mem.h"
#include "host_nvs.h"
#include "app_func_trace.h"
#include "imu_pipeline.h"
#include "imu_sample.h"
#include "mt200_host.h"
#include "wifi_host.h"

LOG_MODULE_REGISTER(host_main, LOG_LEVEL_INF);

static atomic_int g_frames;
static atomic_int g_snaps_ok;
static atomic_int g_snaps_fail;
static atomic_int g_notify_sent;
static atomic_bool g_run = true;

static void paint_cubeish(const struct imu_sample *s, const struct attitude_estimator *att)
{
	uint16_t *fb = host_fb_pixels();
	const int cx = HOST_FB_W / 2;
	const int cy = HOST_FB_H / 2;
	const int dx = (int)(att->state.roll * 40.0f);
	const int dy = (int)(att->state.pitch * -40.0f);
	const uint16_t bg = 0x1082;
	const uint16_t fg = (uint16_t)(0xF800 - (uint16_t)(s->az * 200.0f));

	host_fb_lock();
	for (int i = 0; i < HOST_FB_W * HOST_FB_H; i++) {
		fb[i] = bg;
	}
	for (int y = -20; y <= 20; y++) {
		for (int x = -20; x <= 20; x++) {
			const int px = cx + dx + x;
			const int py = cy + dy + y;

			if (px >= 0 && px < HOST_FB_W && py >= 0 && py < HOST_FB_H) {
				fb[py * HOST_FB_W + px] = fg;
			}
		}
	}
	host_fb_unlock();
}

static void *render_thread(void *arg)
{
	(void)arg;
	host_debug_set_thread_name("render");
	LOG_INF("render thread enter");

	while (atomic_load(&g_run)) {
		struct imu_sample s;
		struct attitude_estimator att;

		if (imu_pipeline_snapshot(&s, &att)) {
			atomic_fetch_add(&g_snaps_ok, 1);
			paint_cubeish(&s, &att);
			atomic_fetch_add(&g_frames, 1);
			if (ble_host_notify_enabled()) {
				ble_host_enqueue_sample(&s, k_uptime_get_32());
				atomic_fetch_add(&g_notify_sent, 1);
			}
			host_fb_sdl_present();
			/* Holds ext-bus + renderer_busy for the desk flush (~73ms on v322). */
			host_panel_flush();
			host_latency_ms(HOST_LAT_RENDER_REMAINDER_MS);
		} else {
			atomic_fetch_add(&g_snaps_fail, 1);
			HOST_LOG(HOST_LOG_DBG, "host_main", "snapshot miss (recovering?)");
			host_latency_ms(HOST_LAT_RENDER_REMAINDER_MS);
		}
		/* Floor sleep when HOST_NO_LATENCY=1 — otherwise miss-path busy-spins and
		 * starves imu_wq under Valgrind (IMU never becomes live). */
		if (!host_latency_enabled()) {
			k_msleep(5);
		}
	}

	LOG_INF("render thread leave frames=%d", atomic_load(&g_frames));
	return NULL;
}

static void *ble_looper_thread(void *arg)
{
	(void)arg;
	host_debug_set_thread_name("ble_loop");
	LOG_INF("ble looper enter");

	while (atomic_load(&g_run)) {
		struct imu_sample batch[8];
		const size_t n = ble_host_drain_notify(batch, 8);

		if (n > 0) {
			HOST_LOG(HOST_LOG_DBG, "host_main", "NOTIFY drain n=%zu az0=%.3f drops=%u",
				 n, (double)batch[0].az, ble_host_notify_drops());
		}
		ble_host_script_tick(k_uptime_get_32());
		mt200_host_tick(k_uptime_get_32());
		wifi_host_tick();
		k_msleep(15);
	}
	LOG_INF("ble looper leave queued=%u", ble_host_notify_queued());
	return NULL;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  --seconds N          run duration (default 8)\n"
		"  --virtual-time       advance clock without sleeping\n"
		"  --stress-recover     hammer imu_pipeline_request_recover\n"
		"  --log-level LVL      err|wrn|inf|dbg|trace|func|all|0..10\n"
		"  --nvs-path PATH      file-backed NVS/crash-ring (default mocks_nvs.bin)\n"
		"  --no-sdl             force framebuffer-only (also HOST_NO_SDL=1)\n"
		"  --sdl                request SDL if compiled + display present\n"
		"  --no-flat-heap       use libc malloc (also HOST_NO_FLAT_HEAP=1)\n"
		"  --flat-heap          force ESP32 DRAM+PSRAM arena allocator\n"
		"\n"
		"HOST_NO_LATENCY=1 skips stub usleep/msleep calibrated from ttyACM0.log.\n"
		"SDL auto-skips on headless (no DISPLAY/WAYLAND_DISPLAY) so CI stays green.\n"
		"Flat heap maps 512KiB+8MiB and serves malloc/k_malloc from it (not under ASan wrap).\n",
		argv0);
}

int main(int argc, char **argv)
{
	int seconds = 8;
	bool virtual_time = false;
	bool stress_recover = false;
	int log_level = HOST_LOG_INF;
	const char *nvs_path = "mocks_nvs.bin";
	bool sdl_want = true;
	bool flat_heap_want = true;

	/* Parse before any logging/alloc so --no-flat-heap wins over early wraps. */
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
			seconds = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--virtual-time")) {
			virtual_time = true;
		} else if (!strcmp(argv[i], "--stress-recover")) {
			stress_recover = true;
		} else if (!strcmp(argv[i], "--log-level") && i + 1 < argc) {
			const int parsed = host_debug_parse_level(argv[++i]);

			if (parsed < 0) {
				fprintf(stderr, "bad --log-level '%s'\n", argv[i]);
				return 1;
			}
			log_level = parsed;
		} else if (!strcmp(argv[i], "--nvs-path") && i + 1 < argc) {
			nvs_path = argv[++i];
		} else if (!strcmp(argv[i], "--no-sdl")) {
			sdl_want = false;
		} else if (!strcmp(argv[i], "--sdl")) {
			sdl_want = true;
		} else if (!strcmp(argv[i], "--no-flat-heap")) {
			flat_heap_want = false;
		} else if (!strcmp(argv[i], "--flat-heap")) {
			flat_heap_want = true;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(argv[0]);
			return 0;
		} else {
			fprintf(stderr, "unknown arg '%s'\n", argv[i]);
			usage(argv[0]);
			return 1;
		}
	}

	host_mem_set_wanted(flat_heap_want);
	(void)host_mem_init();
	host_debug_init();
	host_debug_set_thread_name("main");
	host_debug_set_level(log_level);
	if (getenv("HOST_FUNC_TRACE") && getenv("HOST_FUNC_TRACE")[0] == '1') {
		host_func_trace_set(true);
		HOST_LOG(HOST_LOG_ERR, "host_main", "HOST_FUNC_TRACE=1 — ERR enter/leave on");
	}
	host_latency_init();
	P_DBG(DP_ALWAYS, "handshake_host boot log_level=%d sdl_want=%d flat_heap=%d", log_level,
	      sdl_want ? 1 : 0, host_mem_enabled() ? 1 : 0);

	if (virtual_time) {
		host_time_set_virtual(true);
	}

	host_nvs_init(nvs_path);
	{
		char boot_note[64];
		const int n = snprintf(boot_note, sizeof(boot_note), "boot@%llu",
				       (unsigned long long)host_debug_uptime_ms());

		(void)host_crash_ring_append(boot_note, (size_t)n + 1);
		LOG_INF("crash_ring slots=%zu", host_crash_ring_count());
	}

	wifi_host_init();
	mt200_host_init();
	ble_host_init();
	/* Desk: adv→conn ~0.6–2s, conn→NOTIFY med 1413ms (ttyACM0). */
	ble_host_script_arm(HOST_LAT_BLE_SCRIPT_CONN_MS, HOST_LAT_BLE_SCRIPT_NOTIFY_MS);

	host_fb_sdl_set_wanted(sdl_want);
	(void)host_fb_sdl_init();

	LOG_INF("handshake_host start (%ds)%s%s sdl=%s", seconds,
		virtual_time ? " virtual-time" : "", stress_recover ? " stress-recover" : "",
		host_fb_sdl_enabled() ? "on" : "off");

	if (!imu_pipeline_init()) {
		LOG_ERR("imu_pipeline_init failed");
		host_fb_sdl_shutdown();
		return 1;
	}
	imu_pipeline_start();

	pthread_t rend;
	pthread_t ble;
	if (pthread_create(&rend, NULL, render_thread, NULL) != 0 ||
	    pthread_create(&ble, NULL, ble_looper_thread, NULL) != 0) {
		LOG_ERR("pthread_create failed");
		host_fb_sdl_shutdown();
		return 1;
	}

	const int64_t t0 = k_uptime_get();
	uint32_t hb = 0;
	int64_t last_hb_log = t0;
	int64_t last_stress = 0;

	while ((k_uptime_get() - t0) < (int64_t)seconds * 1000) {
		imu_pipeline_poll();
		const uint32_t ticks = imu_pipeline_take_hb_ticks();
		hb += ticks;

		const int64_t now = k_uptime_get();
		if (now - last_hb_log >= 1000) {
			LOG_INF("hb ready=%d recovering=%d ticks=%u frames=%d snap_ok=%d fail=%d "
				"ble=%d notify=%d sent=%d drops=%u",
				imu_pipeline_ready() ? 1 : 0, imu_pipeline_recovering() ? 1 : 0, hb,
				atomic_load(&g_frames), atomic_load(&g_snaps_ok),
				atomic_load(&g_snaps_fail), ble_host_connected() ? 1 : 0,
				ble_host_notify_enabled() ? 1 : 0, atomic_load(&g_notify_sent),
				ble_host_notify_drops());
			if (imu_pipeline_ready()) {
				imu_pipeline_log_motion();
			}
			last_hb_log = now;
		}

		/* One recover request every 1.5s after warm-up — not every poll. */
		if (stress_recover && (now - t0) > 2000 && (now - last_stress) >= 1500) {
			last_stress = now;
			LOG_WRN("stress: request_recover");
			imu_pipeline_request_recover();
		}

		if (virtual_time) {
			host_time_advance_ms(5);
			sched_yield();
		} else {
			k_msleep(5);
		}
	}

	atomic_store(&g_run, false);
	pthread_join(rend, NULL);
	pthread_join(ble, NULL);
	host_fb_sdl_shutdown();
	host_mem_log_stats("exit");

	LOG_INF("done ready=%d recovering=%d hb=%u frames=%d notify_sent=%d crash_ring=%zu",
		imu_pipeline_ready() ? 1 : 0, imu_pipeline_recovering() ? 1 : 0, hb,
		atomic_load(&g_frames), atomic_load(&g_notify_sent), host_crash_ring_count());

	if (stress_recover) {
		LOG_INF("stress mode — accept mid-recover exit");
		return 0;
	}
	if (!imu_pipeline_ready() || hb == 0) {
		LOG_ERR("IMU never became live");
		return 2;
	}
	if (!ble_host_connected() || !ble_host_notify_enabled()) {
		LOG_ERR("BLE script did not reach NOTIFY");
		return 3;
	}
	return 0;
}
