#include <math.h>

#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>

#include "attitude.h"
#include "app_func_trace.h"
#include "device_config.h"
#include "floor_calib.h"
#include "imu_cal.h"
#include "imu_pipeline.h"
#include "imu_sample.h"
#include "power_manager.h"
#include "qmi8658.h"
#include "renderer.h"
#include "scene_zoom.h"
#include "stack_ra_check.h"
#include "stall_watchdog.h"
#include "crash_alive.h"
#include "vibro_capture.h"
#include "walk_distance.h"

LOG_MODULE_REGISTER(imu_pipe, LOG_LEVEL_INF);

#define IMU_CAL_SAMPLES     80
#define IMU_STILL_ABSORB_N  64
#define IMU_FAIL_RECOVER    25U
#define IMU_RECOVER_RETRY_MS 2000U
#define IMU_RECOVER_MAX_BACKOFF_MS 60000U
/* Host *san/valgrind: wall clock >> I2C; keep soak alive (desk stays 20s). */
#if defined(CONFIG_HOST_MOCK)
#define IMU_RECOVER_STUCK_MS 900000
#else
#define IMU_RECOVER_STUCK_MS 20000
#endif
#define IMU_WQ_STACK        6144

static K_THREAD_STACK_DEFINE(imu_wq_stack, IMU_WQ_STACK);
static struct k_work_q imu_wq;
static struct k_work_delayable g_recover_work;
static K_MUTEX_DEFINE(imu_lock);
static bool g_wq_started;
static bool g_started;
static int64_t g_poll_next_ms;
static uint8_t g_recover_fail_streak;
static int64_t g_recover_backoff_until_ms;
static int64_t g_recover_started_ms;

static struct imu_calibration g_cal;
static struct imu_sample g_latest;
static struct imu_pipeline_raw_entry g_raw_ring[IMU_PIPELINE_RAW_RING_CAP];
static uint8_t g_raw_ring_head;
static uint8_t g_raw_ring_count;
static struct attitude_estimator g_attitude = { .beta = 0.1f };
static struct walk_distance_estimator g_walk;
/* Published under imu_lock; walk_distance_m() may return stale without blocking render. */
static float g_walk_m_pub;
static float g_accel_scale = 1.0f;
static float g_gyro_scale = 1.0f;
/* Cross-thread status flags — poll/snapshot/BLE read without taking imu_lock. */
static atomic_t g_ready;
static atomic_t g_live;
static atomic_t g_recovering;
static uint32_t g_fail_streak;
static uint32_t g_last_imu_ms;
static uint32_t g_hb_ticks;
static uint32_t g_dup_streak;
static uint32_t g_dup_window;
static struct imu_sample g_prev_sample;
static bool g_have_prev_sample;
static float g_still_sx;
static float g_still_sy;
static float g_still_sz;
static float g_still_qxx;
static float g_still_qyy;
static float g_still_qzz;
static uint16_t g_still_n;
static int64_t g_cal_done_ms;

static bool read_uncalibrated(struct imu_sample *out);

static void apply_imu_scale(struct imu_sample *sample)
{
	sample->ax *= g_accel_scale;
	sample->ay *= g_accel_scale;
	sample->az *= g_accel_scale;
	sample->gx *= g_gyro_scale;
	sample->gy *= g_gyro_scale;
	sample->gz *= g_gyro_scale;
}

static void load_config_from_device(void)
{
	const struct device_config_v1 *cfg = device_config_runtime();
	struct walk_distance_config wcfg = {
		.user_height_m = cfg->walk_height_m,
		.pocket_height_m = cfg->walk_pocket_m,
		.step_min_m = cfg->walk_step_min_m,
		.step_max_m = cfg->walk_step_max_m,
	};

	walk_distance_set_config(&g_walk, &wcfg);
	walk_distance_set_surface(&g_walk, (enum walk_surface)cfg->walk_surface);
	g_accel_scale = cfg->imu_accel_scale > 0.0f ? cfg->imu_accel_scale : 1.0f;
	g_gyro_scale = cfg->imu_gyro_scale > 0.0f ? cfg->imu_gyro_scale : 1.0f;
	vibro_capture_apply_config(cfg);
}

static void still_reset(void)
{
	g_still_n = 0;
	g_still_sx = 0.0f;
	g_still_sy = 0.0f;
	g_still_sz = 0.0f;
	g_still_qxx = 0.0f;
	g_still_qyy = 0.0f;
	g_still_qzz = 0.0f;
}

static void absorb_still_gyro_bias(const struct imu_sample *post_cal)
{
	const float gx = post_cal->gx;
	const float gy = post_cal->gy;
	const float gz = post_cal->gz;
	const float ax = post_cal->ax;
	const float ay = post_cal->ay;
	const float az = post_cal->az;
	const float gmag = sqrtf(gx * gx + gy * gy + gz * gz);
	const float amag = sqrtf(ax * ax + ay * ay + az * az);

	/* Failed boot cal leaves a few dps of residual — allow a short window to
	 * swallow that, then only fine-trim temperature drift. */
	const float gmax = (k_uptime_get() - g_cal_done_ms < 4000) ? 16.0f : 1.2f;

	if (gmag > gmax || fabsf(amag - 1.0f) > 0.12f) {
		still_reset();
		return;
	}

	g_still_sx += gx;
	g_still_sy += gy;
	g_still_sz += gz;
	g_still_qxx += gx * gx;
	g_still_qyy += gy * gy;
	g_still_qzz += gz * gz;
	g_still_n++;
	if (g_still_n < IMU_STILL_ABSORB_N) {
		return;
	}

	const float n = (float)g_still_n;
	const float dx = g_still_sx / n;
	const float dy = g_still_sy / n;
	const float dz = g_still_sz / n;
	float vx = g_still_qxx / n - dx * dx;
	float vy = g_still_qyy / n - dy * dy;
	float vz = g_still_qzz / n - dz * dz;

	if (vx < 0.0f) {
		vx = 0.0f;
	}
	if (vy < 0.0f) {
		vy = 0.0f;
	}
	if (vz < 0.0f) {
		vz = 0.0f;
	}

	still_reset();

	/* Don't chase a noisy window — that walks the offset and wobbles the cube. */
	if (sqrtf(vx) > 2.0f || sqrtf(vy) > 2.0f || sqrtf(vz) > 2.0f) {
		return;
	}

	g_cal.gyro_off_x += dx;
	g_cal.gyro_off_y += dy;
	g_cal.gyro_off_z += dz;
	g_cal.gyro_done = true;

	if (fabsf(dx) > 0.20f || fabsf(dy) > 0.20f || fabsf(dz) > 0.20f) {
		LOG_INF("gyro still-trim d=%.3f %.3f %.3f now=%.3f %.3f %.3f", (double)dx,
			(double)dy, (double)dz, (double)g_cal.gyro_off_x, (double)g_cal.gyro_off_y,
			(double)g_cal.gyro_off_z);
	}
}

static void wait_gyro_stable(void)
{
	for (int w = 0; w < 6; w++) {
		float sx = 0.0f;
		float sy = 0.0f;
		float sz = 0.0f;
		float qxx = 0.0f;
		float qyy = 0.0f;
		float qzz = 0.0f;
		int n = 0;

		for (int i = 0; i < 25; i++) {
			struct imu_sample s;

			if (!read_uncalibrated(&s)) {
				k_msleep(5);
				continue;
			}
			sx += s.gx;
			sy += s.gy;
			sz += s.gz;
			qxx += s.gx * s.gx;
			qyy += s.gy * s.gy;
			qzz += s.gz * s.gz;
			n++;
			k_msleep(8);
		}
		stall_watchdog_feed_main();
		if (n < 16) {
			continue;
		}

		const float fn = (float)n;
		const float mx = sx / fn;
		const float my = sy / fn;
		const float mz = sz / fn;
		float vx = qxx / fn - mx * mx;
		float vy = qyy / fn - my * my;
		float vz = qzz / fn - mz * mz;

		if (vx < 0.0f) {
			vx = 0.0f;
		}
		if (vy < 0.0f) {
			vy = 0.0f;
		}
		if (vz < 0.0f) {
			vz = 0.0f;
		}

		const float rx = sqrtf(vx);
		const float ry = sqrtf(vy);
		const float rz = sqrtf(vz);

		if (rx < 2.5f && ry < 2.5f && rz < 2.5f) {
			LOG_INF("gyro settled rms=%.2f %.2f %.2f", (double)rx, (double)ry,
				(double)rz);
			return;
		}
		LOG_INF("gyro settling rms=%.2f %.2f %.2f", (double)rx, (double)ry, (double)rz);
	}
}

static bool read_uncalibrated(struct imu_sample *out)
{
	struct qmi8658_sample raw;

	if (!qmi8658_read(&raw)) {
		return false;
	}

	out->ax = raw.ax;
	out->ay = raw.ay;
	out->az = raw.az;
	out->gx = raw.gx;
	out->gy = raw.gy;
	out->gz = raw.gz;
	out->temp_c = raw.temp_c;
	return true;
}

static bool init_hw_and_calibrate(void)
{
	APP_ENTER();
	const int init_rc = qmi8658_ready() ? qmi8658_reinit() : qmi8658_init();

	if (init_rc != 0) {
		LOG_ERR("QMI8658 init failed (%d)", init_rc);
		APP_LEAVE();
		return false;
	}

	wait_gyro_stable();
	LOG_INF("Calibrating gyro — keep board still...");
	if (!imu_cal_gyro(&g_cal, read_uncalibrated, IMU_CAL_SAMPLES)) {
		LOG_WRN("Gyro calibration rejected (moving or not ready) — retry");
		k_msleep(150);
		if (!imu_cal_gyro(&g_cal, read_uncalibrated, IMU_CAL_SAMPLES)) {
			LOG_WRN("Gyro calibration incomplete — residual bias will spin yaw");
		} else {
			LOG_INF("Gyro calibration OK (retry) off=%.3f %.3f %.3f",
				(double)g_cal.gyro_off_x, (double)g_cal.gyro_off_y,
				(double)g_cal.gyro_off_z);
		}
	} else {
		LOG_INF("Gyro calibration OK off=%.3f %.3f %.3f", (double)g_cal.gyro_off_x,
			(double)g_cal.gyro_off_y, (double)g_cal.gyro_off_z);
	}

	if (floor_calib_valid()) {
		g_cal.accel_off_x = 0.0f;
		g_cal.accel_off_y = 0.0f;
		g_cal.accel_off_z = 0.0f;
		g_cal.accel_done = false;
		LOG_INF("Accel level-zero skipped (floor calib already maps gravity)");
	} else {
		LOG_INF("Calibrating accel — board flat...");
		if (!imu_cal_accel_level(&g_cal, read_uncalibrated, IMU_CAL_SAMPLES)) {
			LOG_WRN("Accel calibration skipped (not flat or incomplete)");
		} else {
			LOG_INF("Accel calibration OK");
		}
	}

	still_reset();
	g_cal_done_ms = k_uptime_get();

	attitude_reset(&g_attitude);
	g_fail_streak = 0;

	{
		struct imu_sample first;

		if (imu_pipeline_read_raw(&first)) {
			k_mutex_lock(&imu_lock, K_FOREVER);
			g_latest = first;
			atomic_set(&g_ready, 1);
			atomic_set(&g_live, 1);
			k_mutex_unlock(&imu_lock);
			LOG_INF("IMU ax=%.3f ay=%.3f az=%.3f gx=%.2f gy=%.2f gz=%.2f",
				(double)first.ax, (double)first.ay, (double)first.az,
				(double)first.gx, (double)first.gy, (double)first.gz);
		} else {
			k_mutex_lock(&imu_lock, K_FOREVER);
			atomic_set(&g_ready, 1);
			atomic_set(&g_live, 1);
			k_mutex_unlock(&imu_lock);
			LOG_WRN("IMU first sample missing after cal");
		}
	}

	APP_LEAVE();
	return true;
}

static void recover_work_fn(struct k_work *work)
{
	bool ok;

	APP_ENTER();
	ARG_UNUSED(work);

	/*
	 * Defer bring-up I2C while panel SPI owns the MSPI/cache path.
	 * v311–v313: recover overlapping flush → DoubleException (Invalid SP).
	 * Wait up to ~2s; re-check once more right before init_hw.
	 */
	for (int i = 0; i < 400 && renderer_busy(); i++) {
		k_msleep(5);
	}
	if (renderer_busy()) {
		LOG_WRN("IMU recover deferred — panel still busy");
		k_work_schedule_for_queue(&imu_wq, &g_recover_work, K_MSEC(200));
		APP_LEAVE();
		return;
	}

	k_mutex_lock(&imu_lock, K_FOREVER);
	atomic_set(&g_recovering, 1);
	atomic_set(&g_ready, 0);
	atomic_set(&g_live, 0);
	g_recover_started_ms = k_uptime_get();
	k_mutex_unlock(&imu_lock);

	LOG_INF("IMU recover attempt");
	stall_watchdog_feed_main();

	/* Do not hold imu_lock during I2C calibrate — render/BLE need snapshot. */
	ok = init_hw_and_calibrate();
	stall_watchdog_feed_main();

	k_mutex_lock(&imu_lock, K_FOREVER);
	g_recover_started_ms = 0;
	if (ok) {
		LOG_INF("IMU recover OK");
		atomic_set(&g_recovering, 0);
		g_recover_fail_streak = 0U;
		g_recover_backoff_until_ms = 0;
		g_poll_next_ms = 0;
		k_mutex_unlock(&imu_lock);
		APP_LEAVE();
		return;
	}

	atomic_set(&g_ready, 0);
	atomic_set(&g_live, 0);
	atomic_set(&g_recovering, 0);
	if (g_recover_fail_streak < 8U) {
		g_recover_fail_streak++;
	}
	{
		uint32_t delay = IMU_RECOVER_RETRY_MS << (g_recover_fail_streak - 1U);

		if (delay > IMU_RECOVER_MAX_BACKOFF_MS) {
			delay = IMU_RECOVER_MAX_BACKOFF_MS;
		}
		g_recover_backoff_until_ms = k_uptime_get() + (int64_t)delay;
		k_mutex_unlock(&imu_lock);
		LOG_WRN("IMU recover failed — next try in %ums", delay);
		k_work_schedule_for_queue(&imu_wq, &g_recover_work, K_MSEC(delay));
		APP_LEAVE();
		return;
	}
}

static void imu_poll_once(void)
{
	STACK_RA_CHECK_SETUP;
	const int64_t now = k_uptime_get();
	float dt = 0.01f;

	if (atomic_get(&g_recovering) || !atomic_get(&g_ready)) {
		return;
	}
	/* Extra gate — tick also checks, but skip scheduling while SPI owns bus. */
	if (renderer_busy()) {
		return;
	}
	STACK_RA_CHECK();

	/* Timed lock — K_FOREVER here wedged main at imu_poll (v306 ~54m hang). */
	if (k_mutex_lock(&imu_lock, K_MSEC(50)) != 0) {
		return;
	}
	if (now < g_poll_next_ms) {
		k_mutex_unlock(&imu_lock);
		return;
	}
	g_poll_next_ms = now + (int64_t)power_manager_imu_interval_ms();
	if (g_last_imu_ms != 0U) {
		dt = (float)(now - (int64_t)g_last_imu_ms) / 1000.0f;
		if (dt > 0.025f) {
			dt = 0.01f;
		}
	}
	g_last_imu_ms = (uint32_t)now;
	k_mutex_unlock(&imu_lock);

	if (imu_pipeline_tick(dt)) {
		/* hb under lock not required for host; keep simple increment */
		g_hb_ticks++;
	}
}

void imu_pipeline_poll(void)
{
	STACK_RA_CHECK_SETUP;
	if (atomic_get(&g_recovering)) {
		int64_t started;

		k_mutex_lock(&imu_lock, K_FOREVER);
		started = g_recover_started_ms;
		k_mutex_unlock(&imu_lock);

		if (started != 0) {
			const int64_t stuck_ms = k_uptime_get() - started;

			if (stuck_ms >= IMU_RECOVER_STUCK_MS) {
				LOG_ERR("IMU recover stuck %lldms — cold reboot",
					(long long)stuck_ms);
				k_msleep(80);
				crash_alive_mark_clean_shutdown();
				sys_reboot(SYS_REBOOT_COLD);
			}
		}
	}

	STACK_RA_CHECK();
	imu_poll_once();
	STACK_RA_CHECK();
	if (!renderer_busy()) {
		floor_calib_poll();
	}
	STACK_RA_CHECK();
}

uint32_t imu_pipeline_take_hb_ticks(void)
{
	const uint32_t n = g_hb_ticks;

	g_hb_ticks = 0;
	return n;
}

void imu_pipeline_log_motion(void)
{
	struct imu_sample s;
	struct attitude_estimator att;
	const uint32_t dups = g_dup_window;
	const uint32_t streak = g_dup_streak;

	g_dup_window = 0;

	if (!imu_pipeline_snapshot(&s, &att)) {
		LOG_DBG("motion snapshot-fail dup=%u streak=%u", dups, streak);
		return;
	}

	const float *z = scene_zoom_current();

	/* DBG: periodic under LOG_IMMEDIATE + usb_serial wedges CDC (silent soak). */
	LOG_DBG("motion ax=%.2f ay=%.2f az=%.2f gx=%.1f gy=%.1f gz=%.1f "
		"rpy=%.1f %.1f %.1f zoom=%.2f dup=%u streak=%u",
		(double)s.ax, (double)s.ay, (double)s.az, (double)s.gx, (double)s.gy,
		(double)s.gz, (double)(att.state.roll * 57.29578f),
		(double)(att.state.pitch * 57.29578f), (double)(att.state.yaw * 57.29578f),
		z != NULL ? (double)z[0] : 0.0, dups, streak);
}

void imu_pipeline_request_recover(void)
{
	if (atomic_get(&g_recovering)) {
		return;
	}

	k_mutex_lock(&imu_lock, K_FOREVER);
	if (g_recover_fail_streak > 0U && k_uptime_get() < g_recover_backoff_until_ms) {
		k_mutex_unlock(&imu_lock);
		return;
	}
	atomic_set(&g_ready, 0);
	atomic_set(&g_live, 0);
	k_mutex_unlock(&imu_lock);
	k_work_schedule_for_queue(&imu_wq, &g_recover_work, K_NO_WAIT);
}

void imu_pipeline_reschedule(void)
{
	g_poll_next_ms = 0;
}

void imu_pipeline_start(void)
{
	APP_ENTER();
	if (!g_wq_started) {
		APP_LEAVE();
		return;
	}

	/* After BLE + power_manager_mark_ready so I2C does not race CPU PLL retap /
	 * BT controller bring-up (that wedge left g_recovering stuck for minutes). */
	g_started = true;
	k_work_schedule_for_queue(&imu_wq, &g_recover_work, K_MSEC(300));
	LOG_INF("IMU bring-up scheduled (post BLE/CPU settle)");
	APP_LEAVE();
}

bool imu_pipeline_started(void)
{
	return g_started;
}

bool imu_pipeline_init(void)
{
	APP_ENTER();
	struct walk_distance_config wcfg = {
		.user_height_m = 1.80f,
		.pocket_height_m = 1.50f,
		.step_min_m = 0.70f,
		.step_max_m = 0.90f,
	};

	k_work_queue_start(&imu_wq, imu_wq_stack, K_THREAD_STACK_SIZEOF(imu_wq_stack), 6, NULL);
	g_wq_started = true;
	k_work_init_delayable(&g_recover_work, recover_work_fn);

	walk_distance_init(&g_walk, &wcfg);
	vibro_capture_init();
	floor_calib_init();
	load_config_from_device();

	/*
	 * Do not block main (task WDT) on I2C gyro settle/calibrate. Field boards were
	 * rebooting at ~17.7s (MAIN_WDT 15s + boot) when probe/cal hung under dual BLE.
	 * Bring-up is scheduled from imu_pipeline_start() after BLE/CPU settle.
	 */
	atomic_set(&g_ready, 0);
	atomic_set(&g_live, 0);
	g_recover_fail_streak = 0U;
	g_recover_backoff_until_ms = 0;
	g_recover_started_ms = 0;
	g_last_imu_ms = 0;
	g_poll_next_ms = 0;
	LOG_INF("IMU pipeline ready (bring-up deferred until imu_pipeline_start)");
	APP_LEAVE();
	return true;
}

void imu_pipeline_apply_config(void)
{
	load_config_from_device();
}

bool imu_pipeline_read_raw(struct imu_sample *out)
{
	struct imu_sample sample;

	if (!atomic_get(&g_ready) || out == NULL) {
		return false;
	}

	if (!read_uncalibrated(&sample)) {
		return false;
	}

	imu_cal_apply(&g_cal, &sample);
	absorb_still_gyro_bias(&sample);
	apply_imu_scale(&sample);
	floor_calib_feed(&sample);
	floor_calib_apply(&sample);
	*out = sample;
	/* Do not touch g_latest here — callers that publish must hold imu_lock
	 * (imu_pipeline_tick) or publish under lock after unlock-during-I2C cal. */
	return true;
}

bool imu_pipeline_snapshot(struct imu_sample *sample, struct attitude_estimator *att)
{
	if (sample == NULL) {
		return false;
	}

	if (k_mutex_lock(&imu_lock, K_MSEC(40)) != 0) {
		return false;
	}

	if (!atomic_get(&g_ready)) {
		k_mutex_unlock(&imu_lock);
		return false;
	}

	*sample = g_latest;
	if (att != NULL) {
		*att = g_attitude;
	}
	g_walk_m_pub = walk_distance_state_get(&g_walk)->distance_m;
	k_mutex_unlock(&imu_lock);
	return true;
}

const struct imu_sample *imu_pipeline_latest(void)
{
	return atomic_get(&g_ready) ? &g_latest : NULL;
}

const struct attitude_estimator *imu_pipeline_attitude(void)
{
	return &g_attitude;
}

const struct walk_distance_state *imu_pipeline_walk_state(void)
{
	return walk_distance_state_get(&g_walk);
}

float imu_pipeline_walk_distance_m(void)
{
	/* Never block the render thread forever on IMU I2C holders. */
	if (k_mutex_lock(&imu_lock, K_MSEC(5)) == 0) {
		g_walk_m_pub = walk_distance_state_get(&g_walk)->distance_m;
		k_mutex_unlock(&imu_lock);
	}
	return g_walk_m_pub;
}

bool imu_pipeline_tick(float dt_sec)
{
	STACK_RA_CHECK_SETUP;
	struct imu_sample sample;

	/* Concurrent QMI8658 I2C + panel SPI → LoadProhibited / DoubleException. */
	if (renderer_busy()) {
		return false;
	}

	/* I2C outside imu_lock — holding the lock across qmi8658/I2C made render
	 * wait K_FOREVER on walk_distance/snapshot and starve the frame WDT. */
	if (!atomic_get(&g_ready) || atomic_get(&g_recovering)) {
		return false;
	}

	if (!imu_pipeline_read_raw(&sample)) {
		uint32_t streak;

		k_mutex_lock(&imu_lock, K_FOREVER);
		g_fail_streak++;
		streak = g_fail_streak;
		k_mutex_unlock(&imu_lock);
		if (streak >= IMU_FAIL_RECOVER) {
			LOG_WRN("IMU read fail streak %u — recovering", streak);
			imu_pipeline_request_recover();
		}
		return false;
	}

	STACK_RA_CHECK();

	k_mutex_lock(&imu_lock, K_FOREVER);

	if (!atomic_get(&g_ready) || atomic_get(&g_recovering)) {
		k_mutex_unlock(&imu_lock);
		return false;
	}

	g_fail_streak = 0;
	g_latest = sample;
	attitude_update(&g_attitude, &sample, dt_sec);
	if (g_have_prev_sample && sample.ax == g_prev_sample.ax && sample.ay == g_prev_sample.ay &&
	    sample.az == g_prev_sample.az && sample.gx == g_prev_sample.gx &&
	    sample.gy == g_prev_sample.gy && sample.gz == g_prev_sample.gz) {
		g_dup_streak++;
		g_dup_window++;
	} else {
		g_dup_streak = 0;
		g_have_prev_sample = true;
		g_prev_sample = sample;
	}
	walk_distance_update(&g_walk, &sample, &g_attitude.state, dt_sec, k_uptime_get_32());
	g_walk_m_pub = walk_distance_state_get(&g_walk)->distance_m;

	g_raw_ring[g_raw_ring_head].t_ms = k_uptime_get_32();
	g_raw_ring[g_raw_ring_head].sample = sample;
	g_raw_ring_head = (uint8_t)((g_raw_ring_head + 1U) % IMU_PIPELINE_RAW_RING_CAP);
	if (g_raw_ring_count < IMU_PIPELINE_RAW_RING_CAP) {
		g_raw_ring_count++;
	}

	k_mutex_unlock(&imu_lock);

	STACK_RA_CHECK();

	/* Vibro capture is self-contained; keep it off the IMU mutex. */
	vibro_capture_push(&sample);
	return true;
}

size_t imu_pipeline_drain_raw(struct imu_pipeline_raw_entry *out, size_t max)
{
	size_t n;

	if (out == NULL || max == 0U) {
		return 0U;
	}

	k_mutex_lock(&imu_lock, K_FOREVER);

	n = (size_t)g_raw_ring_count;
	if (n > max) {
		n = max;
	}

	/* Oldest-first: ring holds g_raw_ring_count entries ending right before head. */
	const uint8_t start =
		(uint8_t)((g_raw_ring_head + IMU_PIPELINE_RAW_RING_CAP - g_raw_ring_count) %
			  IMU_PIPELINE_RAW_RING_CAP);
	const size_t skip = (size_t)g_raw_ring_count - n;

	for (size_t i = 0; i < n; i++) {
		const uint8_t idx =
			(uint8_t)((start + skip + i) % IMU_PIPELINE_RAW_RING_CAP);

		out[i] = g_raw_ring[idx];
	}

	g_raw_ring_count = 0U;
	k_mutex_unlock(&imu_lock);
	return n;
}

bool imu_pipeline_live(void)
{
	return atomic_get(&g_ready) && atomic_get(&g_live) && !atomic_get(&g_recovering);
}

bool imu_pipeline_recovering(void)
{
	return atomic_get(&g_recovering) != 0;
}

bool imu_pipeline_ready(void)
{
	return atomic_get(&g_ready) != 0;
}
