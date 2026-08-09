#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

#include "attitude.h"
#include "device_config.h"
#include "imu_cal.h"
#include "imu_pipeline.h"
#include "imu_sample.h"
#include "power_manager.h"
#include "qmi8658.h"
#include "vibro_capture.h"
#include "walk_distance.h"

LOG_MODULE_REGISTER(imu_pipe, LOG_LEVEL_INF);

#define IMU_CAL_SAMPLES     24
#define IMU_FAIL_RECOVER    25U
#define IMU_RECOVER_RETRY_MS 2000U
#define IMU_WQ_STACK        2048

static K_THREAD_STACK_DEFINE(imu_wq_stack, IMU_WQ_STACK);
static struct k_work_q imu_wq;
static struct k_work_delayable g_recover_work;
static K_MUTEX_DEFINE(imu_lock);
static bool g_wq_started;
static int64_t g_poll_next_ms;

static struct imu_calibration g_cal;
static struct imu_sample g_latest;
static struct attitude_estimator g_attitude = { .alpha = 0.15f };
static struct walk_distance_estimator g_walk;
static float g_accel_scale = 1.0f;
static float g_gyro_scale = 1.0f;
static bool g_ready;
static bool g_live;
static bool g_recovering;
static uint32_t g_fail_streak;
static uint32_t g_last_imu_ms;
static uint32_t g_hb_ticks;

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
	const int init_rc = qmi8658_ready() ? qmi8658_reinit() : qmi8658_init();

	if (init_rc != 0) {
		LOG_ERR("QMI8658 init failed (%d)", init_rc);
		return false;
	}

	LOG_INF("Calibrating gyro — keep board still...");
	if (!imu_cal_gyro(&g_cal, read_uncalibrated, IMU_CAL_SAMPLES)) {
		LOG_WRN("Gyro calibration incomplete");
	} else {
		LOG_INF("Gyro calibration OK");
	}

	LOG_INF("Calibrating accel — board flat...");
	if (!imu_cal_accel_level(&g_cal, read_uncalibrated, IMU_CAL_SAMPLES)) {
		LOG_WRN("Accel calibration incomplete");
	} else {
		LOG_INF("Accel calibration OK");
	}

	attitude_reset(&g_attitude);
	g_fail_streak = 0;
	g_ready = true;
	g_live = true;

	if (imu_pipeline_read_raw(&g_latest)) {
		LOG_INF("IMU ax=%.3f ay=%.3f az=%.3f gx=%.2f gy=%.2f gz=%.2f",
			(double)g_latest.ax, (double)g_latest.ay, (double)g_latest.az,
			(double)g_latest.gx, (double)g_latest.gy, (double)g_latest.gz);
	}

	return true;
}

static void recover_work_fn(struct k_work *work)
{
	bool ok;

	ARG_UNUSED(work);

	k_mutex_lock(&imu_lock, K_FOREVER);
	g_recovering = true;
	g_ready = false;
	g_live = false;
	k_mutex_unlock(&imu_lock);

	LOG_INF("IMU recover attempt");

	/* Do not hold imu_lock during I2C calibrate — render/BLE need snapshot. */
	ok = init_hw_and_calibrate();

	k_mutex_lock(&imu_lock, K_FOREVER);
	if (ok) {
		LOG_INF("IMU recover OK");
		g_recovering = false;
		g_poll_next_ms = 0;
		k_mutex_unlock(&imu_lock);
		return;
	}

	g_ready = false;
	g_live = false;
	g_recovering = false;
	k_mutex_unlock(&imu_lock);
	k_work_schedule_for_queue(&imu_wq, &g_recover_work, K_MSEC(IMU_RECOVER_RETRY_MS));
}

static void imu_poll_once(void)
{
	const int64_t now = k_uptime_get();
	float dt = 0.01f;

	if (g_recovering || !g_ready) {
		return;
	}

	if (now < g_poll_next_ms) {
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

	if (imu_pipeline_tick(dt)) {
		g_hb_ticks++;
	}
}

void imu_pipeline_poll(void)
{
	imu_poll_once();
}

uint32_t imu_pipeline_take_hb_ticks(void)
{
	const uint32_t n = g_hb_ticks;

	g_hb_ticks = 0;
	return n;
}

void imu_pipeline_request_recover(void)
{
	if (g_recovering) {
		return;
	}

	k_mutex_lock(&imu_lock, K_FOREVER);
	g_ready = false;
	g_live = false;
	k_mutex_unlock(&imu_lock);
	k_work_schedule_for_queue(&imu_wq, &g_recover_work, K_NO_WAIT);
}

void imu_pipeline_reschedule(void)
{
	g_poll_next_ms = 0;
}

bool imu_pipeline_init(void)
{
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
	load_config_from_device();

	if (!init_hw_and_calibrate()) {
		LOG_WRN("IMU pipeline init failed — auto-retry scheduled");
		k_work_schedule_for_queue(&imu_wq, &g_recover_work, K_MSEC(IMU_RECOVER_RETRY_MS));
		return false;
	}

	g_last_imu_ms = 0;
	g_poll_next_ms = 0;
	return true;
}

void imu_pipeline_apply_config(void)
{
	load_config_from_device();
}

bool imu_pipeline_read_raw(struct imu_sample *out)
{
	struct imu_sample sample;

	if (!g_ready || out == NULL) {
		return false;
	}

	if (!read_uncalibrated(&sample)) {
		return false;
	}

	imu_cal_apply(&g_cal, &sample);
	apply_imu_scale(&sample);
	*out = sample;
	g_latest = sample;
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

	if (!g_ready) {
		k_mutex_unlock(&imu_lock);
		return false;
	}

	*sample = g_latest;
	if (att != NULL) {
		*att = g_attitude;
	}
	k_mutex_unlock(&imu_lock);
	return true;
}

const struct imu_sample *imu_pipeline_latest(void)
{
	return g_ready ? &g_latest : NULL;
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
	float distance_m;

	k_mutex_lock(&imu_lock, K_FOREVER);
	distance_m = walk_distance_state_get(&g_walk)->distance_m;
	k_mutex_unlock(&imu_lock);
	return distance_m;
}

bool imu_pipeline_tick(float dt_sec)
{
	struct imu_sample sample;
	bool ok;

	k_mutex_lock(&imu_lock, K_FOREVER);

	if (!g_ready || g_recovering) {
		k_mutex_unlock(&imu_lock);
		return false;
	}

	if (!imu_pipeline_read_raw(&sample)) {
		g_fail_streak++;
		if (g_fail_streak >= IMU_FAIL_RECOVER) {
			LOG_WRN("IMU read fail streak %u — recovering", g_fail_streak);
			k_mutex_unlock(&imu_lock);
			imu_pipeline_request_recover();
			return false;
		}
		k_mutex_unlock(&imu_lock);
		return false;
	}

	g_fail_streak = 0;
	attitude_update(&g_attitude, &sample, dt_sec);
	walk_distance_update(&g_walk, &sample, &g_attitude.state, dt_sec, k_uptime_get_32());
	vibro_capture_push(&sample);
	ok = true;
	k_mutex_unlock(&imu_lock);
	return ok;
}

bool imu_pipeline_live(void)
{
	return g_ready && g_live && !g_recovering;
}

bool imu_pipeline_recovering(void)
{
	return g_recovering;
}

bool imu_pipeline_ready(void)
{
	return g_ready;
}
