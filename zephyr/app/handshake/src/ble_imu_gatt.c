/*
 * Zephyr BLE IMU GATT — protocol parity with Arduino ble_gatt_provider.cpp
 */

#include <inttypes.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "battery_monitor.h"
#include "board_config.h"
#include "ble_imu_protocol.h"
#include "ble_imu_gatt.h"
#include "ble_looper.h"
#include "chip_temp.h"
#include "clock_sync.h"
#include "crash_report.h"
#include "bist.h"
#include "crash_debug.h"
#include "display_panel.h"
#include "imu_pipeline.h"
#include "power_manager.h"
#include "scene_snapshot.h"
#include "scene_zoom.h"
#include "device_config.h"
#include "vibro_capture.h"
#include "vibro_schedule.h"
#include "radio_scheduler.h"

LOG_MODULE_REGISTER(ble_imu, LOG_LEVEL_INF);

static struct bt_uuid_128 imu_svc_uuid = BT_UUID_INIT_128(BT_UUID_IMU_SVC_VAL);
static struct bt_uuid_128 imu_mode_uuid = BT_UUID_INIT_128(BT_UUID_IMU_MODE_VAL);
static struct bt_uuid_128 imu_status_uuid = BT_UUID_INIT_128(BT_UUID_IMU_STATUS_VAL);
static struct bt_uuid_128 imu_data_uuid = BT_UUID_INIT_128(BT_UUID_IMU_DATA_VAL);
static struct bt_uuid_128 imu_poll_ms_uuid = BT_UUID_INIT_128(BT_UUID_IMU_POLL_MS_VAL);
static struct bt_uuid_128 imu_notify_uuid = BT_UUID_INIT_128(BT_UUID_IMU_NOTIFY_VAL);
static struct bt_uuid_128 imu_time_uuid = BT_UUID_INIT_128(BT_UUID_IMU_TIME_VAL);
static struct bt_uuid_128 imu_caps_uuid = BT_UUID_INIT_128(BT_UUID_IMU_CAPS_VAL);
static struct bt_uuid_128 imu_screen_uuid = BT_UUID_INIT_128(BT_UUID_IMU_SCREEN_VAL);

static uint8_t g_mode = BLE_IMU_MODE_COMPUTED;
static uint16_t g_poll_ms = BLE_IMU_DEFAULT_POLL_MS;
static uint32_t g_seq;
static uint32_t g_caps;
static bool g_notify_enabled;
static bool g_connected;
static bool g_traffic_paused;
static struct bt_conn *g_conn;

static char g_status_json[2][512];
static char g_data_json[2][BLE_IMU_ATT_PAYLOAD_MAX];
static atomic_t g_json_pub_idx;
static size_t g_status_len[2];
static size_t g_data_len[2];

static struct bt_gatt_attr *g_notify_attr;
static uint32_t g_commit_count;
static atomic_t g_need_prep_batch;
static int64_t g_poll_next_ms;
static bool g_poll_armed;

static int json_write_idx(void)
{
	return atomic_get(&g_json_pub_idx) ^ 1;
}

static void json_publish(int idx)
{
	atomic_set(&g_json_pub_idx, idx);
}

static bool g_notify_want;
static int64_t g_time_unix_ms;
static int16_t g_time_tz_min;
static bool g_screen_want;
static atomic_t g_defer_notify;
static atomic_t g_defer_time;
static atomic_t g_defer_mode;
static atomic_t g_defer_screen;
static atomic_t g_defer_poll;
#define BLE_CONNECT_GRACE_MS 12000
/* Defer first adv until IMU/BT settle. No duty-cycle pause — bt_le_adv_stop() wedged sysworkq @ 2min. */
#define BLE_ADV_BOOT_DELAY_MS 8000
#define BLE_ADV_STOP_DEFER_MS 500
#define BLE_ADV_INT_MIN 0x0a00
#define BLE_ADV_INT_MAX 0x0f00

static bool g_adv_boot_delay_done;
static bool g_adv_active;
static bool g_adv_stop_pending;
static int64_t g_adv_start_deadline;
static int64_t g_adv_stop_deadline;

static int64_t g_connect_grace_until;
static bool g_grace_prep_pending;
static atomic_t g_defer_notify_send;

static bool in_connect_grace(void)
{
	return g_connected && k_uptime_get() < g_connect_grace_until;
}

bool ble_imu_in_connect_grace(void)
{
	return in_connect_grace();
}

static float json_safe(float v)
{
	return isfinite(v) ? v : 0.0f;
}

static void append_raw_record(char *dst, size_t dst_size, size_t *len, uint32_t t_ms,
			      const struct imu_sample *s, float dm)
{
	char rec[128];
	int n = snprintf(rec, sizeof(rec),
			 "%s[%" PRIu32 ",%.4f,%.4f,%.4f,%.2f,%.2f,%.2f,%.3f]",
			 (*len > 0) ? "," : "", t_ms, (double)s->ax, (double)s->ay,
			 (double)s->az, (double)s->gx, (double)s->gy, (double)s->gz, (double)dm);

	if (n <= 0 || (size_t)n >= sizeof(rec) || *len + (size_t)n + 1U >= BLE_IMU_COMMIT_BYTES ||
	    *len + (size_t)n >= dst_size) {
		return;
	}
	memcpy(dst + *len, rec, (size_t)n);
	*len += (size_t)n;
}

static void append_computed_record(char *dst, size_t dst_size, size_t *len, uint32_t t_ms,
				   const struct scene_snapshot *snap, const struct mat3 *rot)
{
	char rec[512];
	int n = snprintf(
		rec, sizeof(rec),
		"%s[%" PRIu32 ",%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
		"%.3f,%.3f,%.3f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f]",
		(*len > 0) ? "," : "", t_ms, (double)json_safe(snap->walk_distance_m),
		(double)json_safe(snap->footer_unproject.x),
		(double)json_safe(snap->footer_unproject.y),
		(double)json_safe(snap->footer_unproject.z), (double)scene_zoom_current()[0],
		(double)scene_zoom_current()[1], (double)scene_zoom_current()[2], (double)json_safe(rot->m[0][0]),
		(double)json_safe(rot->m[0][1]), (double)json_safe(rot->m[0][2]),
		(double)json_safe(rot->m[1][0]), (double)json_safe(rot->m[1][1]),
		(double)json_safe(rot->m[1][2]), (double)json_safe(rot->m[2][0]),
		(double)json_safe(rot->m[2][1]), (double)json_safe(rot->m[2][2]),
		(double)snap->axes[0].p0.x, (double)snap->axes[0].p0.y, (double)snap->axes[0].p1.x,
		(double)snap->axes[0].p1.y, (double)snap->axes[1].p0.x, (double)snap->axes[1].p0.y,
		(double)snap->axes[1].p1.x, (double)snap->axes[1].p1.y, (double)snap->axes[2].p0.x,
		(double)snap->axes[2].p0.y, (double)snap->axes[2].p1.x, (double)snap->axes[2].p1.y);

	if (n <= 0 || (size_t)n >= sizeof(rec) || *len + (size_t)n + 1U >= BLE_IMU_COMMIT_BYTES ||
	    *len + (size_t)n >= dst_size) {
		return;
	}
	memcpy(dst + *len, rec, (size_t)n);
	*len += (size_t)n;
}

static void append_scene_record(char *dst, size_t dst_size, size_t *len, uint32_t t_ms,
				const struct scene_snapshot *snap)
{
	char rec[512];
	int n = snprintf(
		rec, sizeof(rec),
		"%s[%" PRIu32 ",%.3f,%.3f,%.3f,%.3f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f",
		(*len > 0) ? "," : "", t_ms, (double)json_safe(snap->walk_distance_m),
		(double)json_safe(snap->footer_unproject.x),
		(double)json_safe(snap->footer_unproject.y),
		(double)json_safe(snap->footer_unproject.z), (double)snap->axes[0].p0.x,
		(double)snap->axes[0].p0.y, (double)snap->axes[0].p1.x, (double)snap->axes[0].p1.y,
		(double)snap->axes[1].p0.x, (double)snap->axes[1].p0.y, (double)snap->axes[1].p1.x,
		(double)snap->axes[1].p1.y, (double)snap->axes[2].p0.x, (double)snap->axes[2].p0.y,
		(double)snap->axes[2].p1.x, (double)snap->axes[2].p1.y);

	if (n <= 0 || (size_t)n >= sizeof(rec)) {
		return;
	}

	for (int i = 0; i < 8; i++) {
		const int m = snprintf(rec + n, sizeof(rec) - (size_t)n, ",%.1f,%.1f",
				       (double)snap->corners[i].x, (double)snap->corners[i].y);

		if (m <= 0) {
			return;
		}
		n += m;
	}

	const int close = snprintf(rec + n, sizeof(rec) - (size_t)n, "]");

	if (close <= 0 || *len + (size_t)n + (size_t)close >= dst_size ||
	    *len + (size_t)n + (size_t)close >= BLE_IMU_COMMIT_BYTES) {
		return;
	}
	n += close;
	memcpy(dst + *len, rec, (size_t)n);
	*len += (size_t)n;
}

static void clamp_json_len(size_t *len, size_t cap)
{
	if (*len >= cap) {
		*len = cap - 1U;
	}
}

static void refresh_json(uint32_t record_count, const char *records, size_t records_len)
{
	struct imu_sample sample;
	const struct battery_state *bat = battery_monitor_state();
	const struct vibro_verdict vib = vibro_capture_verdict();
	const float volts = bat != NULL ? bat->voltage_v : BAT_FULL_V;
	const float pct = bat != NULL ? (float)bat->percent : 100.0f;
	const float trend = bat != NULL ? bat->trend_v : 0.0f;
	const unsigned power_src = (bat != NULL && bat->on_dc) ? 1U : 0U;
	const bool have_sample = imu_pipeline_snapshot(&sample, NULL);

	const int w = json_write_idx();

	int n = snprintf(g_data_json[w], sizeof(g_data_json[w]),
			 "{\"s\":%u,\"m\":%u,\"w\":%d,\"h\":%d,\"n\":%u,\"p\":%u,\"v\":%.2f,"
			 "\"pct\":%u,\"tr\":%.3f,\"d\":[%.*s]}",
			 g_seq, g_mode, PANEL_W, PANEL_H, record_count, power_src, (double)volts,
			 (unsigned)pct, (double)trend, (int)records_len, records);
	if (n < 0) {
		g_data_len[w] = 0;
	} else {
		g_data_len[w] = (size_t)n;
		clamp_json_len(&g_data_len[w], sizeof(g_data_json[w]));
	}

	n = snprintf(g_status_json[w], sizeof(g_status_json[w]),
		     "{\"s\":%u,\"m\":%u,\"n\":%u,\"b\":%u,\"p\":%u,\"v\":%.2f,\"pct\":%u,"
		     "\"tr\":%.3f,\"pp\":%u,\"fw\":\"zephyr\",\"imu\":%u,\"scr\":%u",
		     g_seq, g_mode, record_count, (unsigned)g_data_len[w], power_src, (double)volts,
		     (unsigned)pct, (double)trend, POWER_PROFILE_DC_FULL,
		     imu_pipeline_live() ? 1U : 0U, power_manager_screen_on() ? 1U : 0U);

	if (chip_temp_valid() && n > 0) {
		n += snprintf(g_status_json[w] + n, sizeof(g_status_json[w]) - (size_t)n, ",\"tc\":%.1f",
			      (double)chip_temp_celsius());
	} else if (have_sample && n > 0) {
		n += snprintf(g_status_json[w] + n, sizeof(g_status_json[w]) - (size_t)n, ",\"tc\":%.1f",
			      (double)sample.temp_c);
	}

	if (vib.valid && n > 0) {
		n += snprintf(g_status_json[w] + n, sizeof(g_status_json[w]) - (size_t)n,
			      ",\"vrms\":%.4f,\"vpeak\":%.4f", (double)vib.rms_g,
			      (double)vib.peak_g);
		const struct vibro_edge_features edge = vibro_capture_edge_features();

		if (edge.valid && n > 0) {
			n += snprintf(g_status_json[w] + n, sizeof(g_status_json[w]) - (size_t)n,
				      ",\"cr\":%.3f,\"zcr\":%.2f,\"hfr\":%.3f",
				      (double)edge.crest, (double)edge.zcr_hz,
				      (double)edge.hf_ratio);
		}

		{
			const struct vibro_band_rms bands = vibro_capture_band_rms();

			if (bands.valid && n > 0) {
				n += snprintf(g_status_json[w] + n, sizeof(g_status_json[w]) - (size_t)n,
					      ",\"bnd\":[%.4f,%.4f,%.4f,%.4f]",
					      (double)bands.bands[0], (double)bands.bands[1],
					      (double)bands.bands[2], (double)bands.bands[3]);
			}
		}
	}

	if (vib.valid && n > 0) {
		vibro_capture_on_status_seq(g_seq, vib.has_reference);
	}

	if (vib.has_reference && n > 0) {
		n += snprintf(g_status_json[w] + n, sizeof(g_status_json[w]) - (size_t)n,
			      ",\"ref\":1,\"vd\":%u,\"vcorr\":%.3f,\"vrmsd\":%.4f",
			      (unsigned)vib.level, (double)vib.corr, (double)vib.rms_delta);
		if (vib.has_band_ref) {
			n += snprintf(g_status_json[w] + n, sizeof(g_status_json[w]) - (size_t)n,
				      ",\"bcorr\":%.3f,\"bdmax\":%.3f", (double)vib.band_corr,
				      (double)vib.band_delta_max);
		}
		n += snprintf(g_status_json[w] + n, sizeof(g_status_json[w]) - (size_t)n, ",\"ack\":%u,"
			      "\"opend\":%u",
			      (unsigned)vibro_capture_last_ack_seq(),
			      (unsigned)vibro_capture_pending_offload_count());
	} else if (vibro_capture_pending_offload_count() > 0U && n > 0) {
		const uint32_t psess = vibro_capture_pending_session_seq();

		if (psess > 0U) {
			n += snprintf(g_status_json[w] + n, sizeof(g_status_json[w]) - (size_t)n,
				      ",\"ack\":%u,\"opend\":%u,\"psess\":%u",
				      (unsigned)vibro_capture_last_ack_seq(),
				      (unsigned)vibro_capture_pending_offload_count(), psess);
		} else {
			n += snprintf(g_status_json[w] + n, sizeof(g_status_json[w]) - (size_t)n,
				      ",\"ack\":%u,\"opend\":%u",
				      (unsigned)vibro_capture_last_ack_seq(),
				      (unsigned)vibro_capture_pending_offload_count());
		}
	}

	{
		const struct device_config_v1 *dcfg = device_config_runtime();
		const bool cap_active = vibro_schedule_capture_active(dcfg, clock_sync_now_ms32());
		uint32_t cap_left = 0U;
		uint32_t cap_until = 0U;

		vibro_schedule_window_info(dcfg, clock_sync_now_ms32(), NULL, &cap_left, &cap_until);

		if (n > 0) {
			n += snprintf(g_status_json[w] + n, sizeof(g_status_json[w]) - (size_t)n,
				      ",\"cap\":%u,\"vt\":%u", cap_active ? 1U : 0U,
				      dcfg != NULL ? (unsigned)dcfg->vibro_capture_tier : 0U);
			if (dcfg != NULL && dcfg->reserved[0] >= 2U) {
				const uint32_t interval =
					dcfg->vibro_interval_sec > 0U ? dcfg->vibro_interval_sec : 60U;
				const uint32_t bucket = clock_sync_now_ms32() / 1000U / interval;
				const uint32_t capmix =
					vibro_schedule_effective_window_sec(dcfg, bucket);

				n += snprintf(g_status_json[w] + n, sizeof(g_status_json[w]) - (size_t)n,
					      ",\"capmix\":%u", capmix);
			}
			if (cap_left > 0U || cap_until > 0U) {
				n += snprintf(g_status_json[w] + n, sizeof(g_status_json[w]) - (size_t)n,
					      ",\"capwin\":%u,\"capuntil\":%u", cap_left, cap_until);
			}
			n += snprintf(g_status_json[w] + n, sizeof(g_status_json[w]) - (size_t)n,
				      ",\"cfgseq\":%u,\"locrev\":%u",
				      dcfg != NULL ? dcfg->profile_updated_unix : 0U,
				      dcfg != NULL ? (unsigned)device_config_local_revision(dcfg)
						   : 0U);
		}
	}

	if (n > 0) {
		n += snprintf(g_status_json[w] + n, sizeof(g_status_json[w]) - (size_t)n,
			      ",\"radio\":\"%s\"", radio_scheduler_mode_str());
	}

	if (n > 0) {
		n += snprintf(g_status_json[w] + n, sizeof(g_status_json[w]) - (size_t)n,
			      ",\"rr\":\"%s\"", crash_report_reset_reason_str());
	}

#if defined(CONFIG_APP_CRASH_DEBUG)
	if (n > 0) {
		n += snprintf(g_status_json[w] + n, sizeof(g_status_json[w]) - (size_t)n, ",\"dbg\":1");
		const struct bist_result *br = bist_last();

		if (br != NULL && br->summary[0] != '\0') {
			n += snprintf(g_status_json[w] + n, sizeof(g_status_json[w]) - (size_t)n,
				      ",\"bist\":\"%s\"", br->summary);
		}
	}
#endif

	if (n > 0) {
		n = clock_sync_append_status_json(g_status_json[w], sizeof(g_status_json[w]), n);
	}

	if (n > 0) {
		n += snprintf(g_status_json[w] + n, sizeof(g_status_json[w]) - (size_t)n, "}");
		g_status_len[w] = (size_t)n;
		clamp_json_len(&g_status_len[w], sizeof(g_status_json[w]));
	} else {
		g_status_len[w] = 0;
	}

	json_publish(w);
}

static void build_batch(void)
{
	char records[512];
	size_t records_len = 0;
	uint32_t record_count = 0;
	const uint32_t t_ms = clock_sync_now_ms32();
	struct imu_sample sample;
	struct attitude_estimator att;

	if (!imu_pipeline_snapshot(&sample, &att)) {
		refresh_json(0, "", 0);
		return;
	}

	record_count = 1;
	records[0] = '\0';

	const float walk_m = imu_pipeline_walk_distance_m();

	if (g_mode == BLE_IMU_MODE_RAW) {
		append_raw_record(records, sizeof(records), &records_len, t_ms, &sample, walk_m);
	} else {
		struct scene_snapshot snap = scene_snapshot_build(
			PANEL_W, PANEL_H, scene_zoom_current(), &att.state.rotation, &sample,
			walk_m);

		if (g_mode == BLE_IMU_MODE_SCENE) {
			append_scene_record(records, sizeof(records), &records_len, t_ms, &snap);
		} else {
			append_computed_record(records, sizeof(records), &records_len, t_ms, &snap,
					       &att.state.rotation);
		}
	}

	if (records_len == 0) {
		record_count = 0;
	}

	refresh_json(record_count, records, records_len);
}

static void send_notify(void)
{
	if (!g_notify_enabled || g_notify_attr == NULL || !g_connected) {
		return;
	}

	atomic_set(&g_defer_notify_send, 1);
}

static void schedule_poll_immediate(void);

static void schedule_prep_batch(void)
{
	atomic_set(&g_need_prep_batch, 1);
}

static void schedule_traffic_if_ready(void)
{
	if (in_connect_grace()) {
		return;
	}

	schedule_prep_batch();
	schedule_poll_immediate();
}

static void schedule_poll_immediate(void)
{
	if (in_connect_grace()) {
		g_poll_next_ms = g_connect_grace_until;
	} else {
		g_poll_next_ms = k_uptime_get();
	}

	g_poll_armed = true;
}

static void poll_tick(void)
{
	if (!g_poll_armed || !g_connected || !g_notify_enabled) {
		return;
	}

	const int64_t now = k_uptime_get();

	if (now < g_poll_next_ms) {
		return;
	}

	if (in_connect_grace()) {
		g_poll_next_ms = g_connect_grace_until;
		return;
	}

	if (g_traffic_paused) {
		g_poll_next_ms = now + 500;
		return;
	}

	g_seq++;
	build_batch();
	send_notify();

	g_commit_count++;
	if ((g_commit_count % 30U) == 0U) {
		struct imu_sample s;

		if (imu_pipeline_snapshot(&s, NULL)) {
			LOG_INF("batch s=%u imu=%s ax=%.3f ay=%.3f az=%.3f json=%uB", g_seq,
				imu_pipeline_live() ? "live" : "off", (double)s.ax, (double)s.ay,
				(double)s.az, (unsigned)g_data_len[atomic_get(&g_json_pub_idx)]);
		}
	}

	g_poll_next_ms = now + g_poll_ms;
}

static void notify_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	g_notify_want = (value == BT_GATT_CCC_NOTIFY);
	atomic_set(&g_defer_notify, 1);
}

static ssize_t read_mode(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 void *buf, uint16_t len, uint16_t offset)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &g_mode, sizeof(g_mode));
}

static ssize_t write_mode(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0 || len < 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	g_mode = *(const uint8_t *)buf;
	atomic_set(&g_defer_mode, 1);
	return len;
}

static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   void *buf, uint16_t len, uint16_t offset)
{
	if (in_connect_grace()) {
		static const char grace[] = "{\"s\":0,\"m\":0,\"n\":0,\"scr\":1}";

		return bt_gatt_attr_read(conn, attr, buf, len, offset, grace, sizeof(grace) - 1U);
	}

	const int idx = atomic_get(&g_json_pub_idx);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, g_status_json[idx],
				 g_status_len[idx]);
}

static ssize_t read_data(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 void *buf, uint16_t len, uint16_t offset)
{
	if (in_connect_grace()) {
		static const char grace[] = "{\"s\":0,\"n\":0,\"d\":[]}";

		return bt_gatt_attr_read(conn, attr, buf, len, offset, grace, sizeof(grace) - 1U);
	}

	const int idx = atomic_get(&g_json_pub_idx);

	/*
	 * Do not rebuild JSON here — Android reads DATA on every NOTIFY (~30 Hz).
	 * build_batch() does scene projection + snprintf and wedged the BT stack
	 * after minutes of polling. commit_batch() on the workqueue owns updates.
	 */
	return bt_gatt_attr_read(conn, attr, buf, len, offset, g_data_json[idx], g_data_len[idx]);
}

static ssize_t read_poll_ms(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			    void *buf, uint16_t len, uint16_t offset)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);

	uint8_t le[2];

	sys_put_le16(g_poll_ms, le);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, le, sizeof(le));
}

static ssize_t write_poll_ms(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0 || len < 2) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	const uint8_t *b = buf;
	uint16_t ms = sys_get_le16(b);

	if (ms < BLE_IMU_POLL_MS_MIN) {
		ms = BLE_IMU_POLL_MS_MIN;
	}
	if (ms > BLE_IMU_POLL_MS_MAX) {
		ms = BLE_IMU_POLL_MS_MAX;
	}

	g_poll_ms = ms;
	atomic_set(&g_defer_poll, 1);
	return len;
}

static ssize_t read_notify(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   void *buf, uint16_t len, uint16_t offset)
{
	ARG_UNUSED(attr);

	uint8_t le[4];

	sys_put_le32(g_seq, le);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, le, sizeof(le));
}

static ssize_t write_time(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0 || len < 8) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	int64_t unix_ms = 0;
	int16_t tz_min = clock_sync_tz_offset_min();

	for (int i = 0; i < 8; i++) {
		unix_ms |= (int64_t)((const uint8_t *)buf)[i] << (8 * i);
	}

	if (len >= 12) {
		int32_t tz_raw = 0;

		for (int i = 0; i < 4; i++) {
			tz_raw |= (int32_t)((const uint8_t *)buf)[8 + i] << (8 * i);
		}
		tz_min = (int16_t)tz_raw;
	}

	g_time_unix_ms = unix_ms;
	g_time_tz_min = tz_min;
	atomic_set(&g_defer_time, 1);
	return len;
}

static ssize_t read_caps(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 void *buf, uint16_t len, uint16_t offset)
{
	ARG_UNUSED(attr);

	uint8_t le[4];

	sys_put_le32(g_caps, le);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, le, sizeof(le));
}

static ssize_t read_screen(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   void *buf, uint16_t len, uint16_t offset)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);

	const uint8_t on = power_manager_screen_on() ? 1U : 0U;

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &on, sizeof(on));
}

static ssize_t write_screen(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			    const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0 || len < 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	const bool on = (*(const uint8_t *)buf) != 0U;

	g_screen_want = on;
	atomic_set(&g_defer_screen, 1);
	return len;
}

BT_GATT_SERVICE_DEFINE(
	imu_svc, BT_GATT_PRIMARY_SERVICE(&imu_svc_uuid),
	BT_GATT_CHARACTERISTIC(&imu_mode_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, read_mode, write_mode,
			       NULL),
	BT_GATT_CHARACTERISTIC(&imu_status_uuid.uuid, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, read_status, NULL, NULL),
	BT_GATT_CHARACTERISTIC(&imu_data_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
			       read_data, NULL, NULL),
	BT_GATT_CHARACTERISTIC(&imu_poll_ms_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, read_poll_ms,
			       write_poll_ms, NULL),
	BT_GATT_CHARACTERISTIC(&imu_notify_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_notify, NULL, NULL),
	BT_GATT_CCC(notify_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&imu_time_uuid.uuid, BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE, NULL, write_time, NULL),
	BT_GATT_CHARACTERISTIC(&imu_caps_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
			       read_caps, NULL, NULL),
	BT_GATT_CHARACTERISTIC(&imu_screen_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, read_screen, write_screen,
			       NULL));

static void mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(tx);
	ARG_UNUSED(rx);
}

static struct bt_gatt_cb gatt_cb = {
	.att_mtu_updated = mtu_updated,
};

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_IMU_SVC_VAL),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void connected(struct bt_conn *conn, uint8_t err)
{
	ble_imu_on_connected(conn, err);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ble_imu_on_disconnected(conn, reason);
}

BT_CONN_CB_DEFINE(conn_cb) = {
	.connected = connected,
	.disconnected = disconnected,
};

static int ble_imu_advertise_start_now(void);

static void adv_clear_schedule(void)
{
	g_adv_start_deadline = 0;
}

static void adv_boot_tick(void)
{
	if (g_connected || g_adv_start_deadline <= 0) {
		return;
	}

	if (k_uptime_get() >= g_adv_start_deadline) {
		g_adv_start_deadline = 0;
		(void)ble_imu_advertise_start_now();
	}
}

static void adv_stop_tick(void)
{
	if (!g_adv_stop_pending || !g_adv_active) {
		return;
	}

	if (k_uptime_get() < g_adv_stop_deadline) {
		return;
	}

	g_adv_stop_pending = false;
	const int err = bt_le_adv_stop();

	if (err == 0 || err == -EALREADY) {
		g_adv_active = false;
		LOG_INF("advertising stopped (main looper, post-connect)");
	} else {
		LOG_WRN("adv stop failed (%d)", err);
	}
}

int ble_imu_gatt_init(void)
{
	atomic_set(&g_json_pub_idx, 0);
	g_status_len[0] = 0;
	g_data_len[0] = 0;
	clock_sync_begin();
	g_caps = ble_imu_zephyr_caps();
	build_batch();

	g_notify_attr = bt_gatt_find_by_uuid(imu_svc.attrs, imu_svc.attr_count,
					     &imu_notify_uuid.uuid);
	bt_gatt_cb_register(&gatt_cb);
	adv_clear_schedule();

	return 0;
}

static int ble_imu_advertise_start_now(void)
{
	static const struct bt_le_adv_param adv_param = BT_LE_ADV_PARAM_INIT(
		BT_LE_ADV_OPT_CONNECTABLE | BT_LE_ADV_OPT_USE_IDENTITY, BLE_ADV_INT_MIN,
		BLE_ADV_INT_MAX, NULL);

	int err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (err == -EALREADY) {
		g_adv_active = true;
		return 0;
	}

	if (err) {
		LOG_ERR("advertising failed (%d)", err);
		return err;
	}

	LOG_INF("advertising as \"%s\" (IMU UUID in AD; NET via GATT discovery)", CONFIG_BT_DEVICE_NAME);
	g_adv_active = true;
	return 0;
}

int ble_imu_gatt_looper_adv_start(bool restart)
{
	if (g_connected) {
		return 0;
	}

	if (!restart && !g_adv_boot_delay_done) {
		g_adv_boot_delay_done = true;
		g_adv_start_deadline = k_uptime_get() + BLE_ADV_BOOT_DELAY_MS;
		LOG_INF("BLE adv scheduled in %us (boot settle)", BLE_ADV_BOOT_DELAY_MS / 1000U);
		return 0;
	}

	return ble_imu_advertise_start_now();
}

void ble_imu_on_connected(struct bt_conn *conn, uint8_t err)
{
	(void)ble_looper_post_connected(conn, err);
}

void ble_imu_gatt_set_traffic_paused(bool paused)
{
	if (g_traffic_paused == paused) {
		return;
	}

	g_traffic_paused = paused;
	if (paused) {
		LOG_INF("IMU BLE traffic paused (WiFi radio active)");
	} else if (g_connected && g_notify_enabled) {
		schedule_poll_immediate();
		LOG_INF("IMU BLE traffic resumed");
	}
}

void ble_imu_on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	(void)ble_looper_post_disconnected(reason);
}

void ble_imu_gatt_looper_connected(struct bt_conn *conn)
{
	printk("ble_imu: link up (looper)\n");

	if (g_conn) {
		bt_conn_unref(g_conn);
	}

	g_conn = bt_conn_ref(conn);
	g_connected = true;
	g_connect_grace_until = k_uptime_get() + BLE_CONNECT_GRACE_MS;
	g_poll_armed = false;
	adv_clear_schedule();
	/* Stop adv from main looper — bt_le_adv_stop() on connect path wedged sysworkq. */
	g_adv_stop_pending = g_adv_active;
	g_adv_stop_deadline = k_uptime_get() + BLE_ADV_STOP_DEFER_MS;
	power_manager_set_ble_active(true);
	LOG_INF("connected (grace %ums)", BLE_CONNECT_GRACE_MS);
}

void ble_imu_gatt_looper_disconnected(uint8_t reason)
{
	g_connected = false;
	g_notify_enabled = false;
	g_traffic_paused = false;
	g_connect_grace_until = 0;
	g_grace_prep_pending = false;
	g_poll_armed = false;
	g_adv_stop_pending = false;

	if (g_conn) {
		bt_conn_unref(g_conn);
		g_conn = NULL;
	}

	power_manager_set_ble_active(false);
	LOG_INF("disconnected (%u)", reason);
	(void)ble_imu_gatt_looper_adv_start(true);
}

bool ble_imu_link_active(void)
{
	return g_connected;
}

void ble_imu_gatt_looper_tick(void)
{
	adv_boot_tick();
	adv_stop_tick();

	if (!g_connected) {
		return;
	}

	if (atomic_get(&g_need_prep_batch) != 0 && !in_connect_grace()) {
		atomic_set(&g_need_prep_batch, 0);
		build_batch();
	}

	if (atomic_get(&g_defer_notify) != 0) {
		atomic_set(&g_defer_notify, 0);
		g_notify_enabled = g_notify_want;
		LOG_INF("NOTIFY %s", g_notify_enabled ? "enabled" : "disabled");
		if (g_notify_enabled) {
			if (!in_connect_grace()) {
				schedule_prep_batch();
				schedule_poll_immediate();
			} else {
				g_grace_prep_pending = true;
			}
		} else {
			g_poll_armed = false;
		}
	}

	if (g_grace_prep_pending && g_notify_enabled && !in_connect_grace()) {
		g_grace_prep_pending = false;
		schedule_prep_batch();
		schedule_poll_immediate();
	}

	if (atomic_get(&g_defer_time) != 0) {
		atomic_set(&g_defer_time, 0);
		if (clock_sync_set_from_phone(g_time_unix_ms, g_time_tz_min)) {
			LOG_INF("TIME phone corrected unix_ms=%lld tz=%d",
				(long long)g_time_unix_ms, (int)g_time_tz_min);
		} else {
			LOG_INF("TIME phone check unix_ms=%lld tz=%d drift=%lld ms",
				(long long)g_time_unix_ms, (int)g_time_tz_min,
				(long long)clock_sync_last_drift_ms());
		}
	}

	if (atomic_get(&g_defer_mode) != 0) {
		atomic_set(&g_defer_mode, 0);
		schedule_traffic_if_ready();
	}

	if (atomic_get(&g_defer_poll) != 0) {
		atomic_set(&g_defer_poll, 0);
		schedule_poll_immediate();
	}

	if (atomic_get(&g_defer_screen) != 0) {
		atomic_set(&g_defer_screen, 0);
		power_manager_on_screen(g_screen_want);
		schedule_traffic_if_ready();
	}

	if (atomic_cas(&g_defer_notify_send, 1, 0)) {
		if (g_connected && g_notify_enabled && g_notify_attr != NULL && g_conn != NULL &&
		    !in_connect_grace()) {
			uint8_t payload[4];

			sys_put_le32(g_seq, payload);
			(void)bt_gatt_notify(g_conn, g_notify_attr, payload, sizeof(payload));
		}
	}

	poll_tick();
}
