#include "mt200_bridge.h"
#include "mt200_sample_queue.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "clock_sync.h"
#include "app_func_trace.h"
#include "ble_imu_gatt.h"
#include "radio_scheduler.h"
#include "renderer.h"
#include "stack_ra_check.h"

LOG_MODULE_REGISTER(mt200, LOG_LEVEL_INF);

#if defined(CONFIG_APP_CRASH_DEBUG)

#define MT200_ADDR_STR "25:38:22:92:C9:4E"
/* Past phone advertise + connect window — dual-central scan with an open
 * peripheral link starves render/HCI (frozen cube + stuck phone BLE). */
#define MT200_AUTOSTART_MS 45000U
#define MT200_RESTART_MS 5000U
#define MT200_POLL_MS 60000U
#define MT200_CMD_GAP_MS 500U
#define MT200_SPO2_HOLD_MS 18000U
#define MT200_CMDQ_CAP 8U
/* Veepoo default pairing PIN — confirmDevicePwd("0000", …) before sport/HR reads. */
#define MT200_PWD_DEFAULT 0U
/* H-Band settingTime (A5) re-push while linked — NTP-style drift correction. */
#define MT200_TIME_SYNC_MS (30U * 60U * 1000U)

enum mt200_cmd {
	MT200_CMD_PWD = 1,
	MT200_CMD_SETTIME,
	MT200_CMD_GSENSOR,
	MT200_CMD_HR_START,
	MT200_CMD_HR_STOP,
	MT200_CMD_A8,
	MT200_CMD_D8,
	MT200_CMD_A0,
	MT200_CMD_SPO2_START,
	MT200_CMD_SPO2_STOP,
};

/* clang-format off */
/* Node id must be ULL: bare 0 is int-width on Xtensa and trips -Wshift-count-overflow
 * inside BT_BYTES_LIST_LE48 ((_v) >> 32). */
static struct bt_uuid_128 svc_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xf0080001, 0x0451, 0x4000, 0xb000, 0x000000000000ULL));
static struct bt_uuid_128 notify_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xf0080002, 0x0451, 0x4000, 0xb000, 0x000000000000ULL));
static struct bt_uuid_128 write_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xf0080003, 0x0451, 0x4000, 0xb000, 0x000000000000ULL));
/* clang-format on */

enum disc_state {
	DISC_IDLE = 0,
	DISC_PRIMARY,
	DISC_NOTIFY_CHRC,
	DISC_WRITE_CHRC,
};

static struct bt_conn *volatile g_conn;
static struct bt_gatt_discover_params g_disc;
static struct bt_gatt_subscribe_params g_sub;
static volatile enum disc_state g_state;
static volatile uint16_t g_svc_end_handle;
static volatile uint16_t g_write_handle;
static volatile bool g_active;
static volatile bool g_want_run;
static volatile bool g_paused; /* OTA/WiFi load-shed — start/autostart must not clear this */
static volatile bool g_phone_hold; /* phone peripheral up — MT200 central must stay down */
/*
 * Granulated sessions: scan → connect → fetch within hard timeouts.
 * Quiet gap releases phone advertising + panel SPI (exclusive radio epochs).
 */
enum mt200_phase {
	MT200_PHASE_QUIET = 0,
	MT200_PHASE_SCAN,
	MT200_PHASE_CONNECT,
	MT200_PHASE_FETCH,
};
#define MT200_T_SCAN_MS   15000U
#define MT200_T_CONN_MS   12000U
#define MT200_T_FETCH_MS  75000U
/* On-wrist PPG lock is ~30 s; SpO2 then holds the same LED. */
#define MT200_HR_LOCK_MS  32000U
#define MT200_T_QUIET_MS  (3U * 60U * 1000U) /* longer quiet → less session churn */
#define MT200_QUIET_SPI_SETTLE_MS 10000U /* legacy; quiet now allows continuous SPI */
#define MT200_SESS_NEED_ANY (MT200_FLAG_STEPS | MT200_FLAG_BAT | MT200_FLAG_KCAL)
static volatile uint8_t g_phase = MT200_PHASE_QUIET;
/* GDB-visible breadcrumbs (hang_vars every 300s). */
volatile uint8_t g_mt200_phase;
volatile uint8_t g_mt200_want;
volatile uint8_t g_mt200_busy;
volatile uint8_t g_mt200_quiet_spi_armed;
static uint32_t g_phase_deadline_ms;
static uint32_t g_link_up_ms;
static volatile bool g_sess_ok; /* true → disconnect schedules quiet (success path) */
static uint32_t g_quiet_entered_ms;
static volatile bool g_quiet_spi_done;
static volatile bool g_pwd_ok;
static volatile bool g_pwd_sent;
static struct mt200_telem g_telem;
static volatile uint8_t g_poll_tick;
static volatile int8_t g_rssi = MT200_RSSI_UNAVAIL;
static uint8_t g_cmdq[MT200_CMDQ_CAP];
static volatile uint8_t g_cmdq_rd;
static volatile uint8_t g_cmdq_n;
static volatile bool g_spo2_busy;
static volatile bool g_vitals_done;
static struct k_spinlock g_cmdq_lock;

/*
 * Fill local wall-clock fields used by both confirmDevicePwd (A1) and
 * settingTime (A5). Year is big-endian uint16 (Veepoo "0"+hex(year) pairs).
 */
static void fill_local_tm(struct tm *tm_out, int16_t *tz_min_out)
{
	const int16_t tz_min = clock_sync_tz_offset_min();
	const time_t local_sec =
		(time_t)(clock_sync_now_ms() / 1000LL + (int64_t)tz_min * 60LL);

	gmtime_r(&local_sec, tm_out);
	if (tz_min_out != NULL) {
		*tz_min_out = tz_min;
	}
}

/*
 * confirmDevicePwd packet (vp_bu): opcode A1, PIN as BE u32 low 16 in [1..2],
 * then wall-clock + 24h flag + tz quarters. Without this, A8/D8/A0 are ignored
 * and the watch only emits A1 function/ID frames (wok stays 0).
 */
static void build_pwd_cmd(uint8_t cmd[20])
{
	struct tm tm;
	int16_t tz_min;
	uint16_t year;

	memset(cmd, 0, 20);
	fill_local_tm(&tm, &tz_min);
	year = (uint16_t)(tm.tm_year + 1900);

	cmd[0] = 0xA1;
	cmd[1] = (uint8_t)(MT200_PWD_DEFAULT & 0xFFU); /* pwd LE low (0000) */
	cmd[2] = (uint8_t)((MT200_PWD_DEFAULT >> 8) & 0xFFU);
	cmd[3] = 0x00; /* vp_d mode byte */
	cmd[4] = (uint8_t)((year >> 8) & 0xFFU);
	cmd[5] = (uint8_t)(year & 0xFFU);
	cmd[6] = (uint8_t)(tm.tm_mon + 1);
	cmd[7] = (uint8_t)tm.tm_mday;
	cmd[8] = (uint8_t)tm.tm_hour;
	cmd[9] = (uint8_t)tm.tm_min;
	cmd[10] = (uint8_t)tm.tm_sec;
	cmd[11] = 0x01; /* 24-hour model */
	cmd[12] = 0x01;
	cmd[13] = (uint8_t)(int8_t)(tz_min / 15);
}

/*
 * settingTime / sync_time_oprate (vp_cv): opcode A5 + Y/M/D/h/m/s + time mode.
 * H-Band pushes this after auth and on a timer so the watch clock tracks phone/NTP.
 */
static void build_settime_cmd(uint8_t cmd[20])
{
	struct tm tm;
	uint16_t year;

	memset(cmd, 0, 20);
	fill_local_tm(&tm, NULL);
	year = (uint16_t)(tm.tm_year + 1900);

	cmd[0] = 0xA5;
	cmd[1] = (uint8_t)((year >> 8) & 0xFFU);
	cmd[2] = (uint8_t)(year & 0xFFU);
	cmd[3] = (uint8_t)(tm.tm_mon + 1);
	cmd[4] = (uint8_t)tm.tm_mday;
	cmd[5] = (uint8_t)tm.tm_hour;
	cmd[6] = (uint8_t)tm.tm_min;
	cmd[7] = (uint8_t)tm.tm_sec;
	cmd[8] = 0x01; /* ETimeMode 24-hour */
}

static uint8_t discover_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			    struct bt_gatt_discover_params *params);
static void poll_work_fn(struct k_work *work);
static void restart_work_fn(struct k_work *work);
static void cmd_work_fn(struct k_work *work);
static void spo2_stop_work_fn(struct k_work *work);
static void vitals_spo2_work_fn(struct k_work *work);
static void time_sync_work_fn(struct k_work *work);
static void policy_work_fn(struct k_work *work);
static void sess_watch_fn(struct k_work *work);
static void cmd_enqueue(uint8_t cmd);
static void session_idle(void);
static void session_try_complete(void);
static void session_abort(const char *why);
static void phase_set(uint8_t phase, uint32_t timeout_ms);
static K_WORK_DELAYABLE_DEFINE(g_poll_work, poll_work_fn);
static K_WORK_DELAYABLE_DEFINE(g_restart_work, restart_work_fn);
static K_WORK_DELAYABLE_DEFINE(g_cmd_work, cmd_work_fn);
static K_WORK_DELAYABLE_DEFINE(g_spo2_stop_work, spo2_stop_work_fn);
static K_WORK_DELAYABLE_DEFINE(g_vitals_spo2_work, vitals_spo2_work_fn);
static K_WORK_DELAYABLE_DEFINE(g_time_sync_work, time_sync_work_fn);
static K_WORK_DELAYABLE_DEFINE(g_sess_watch, sess_watch_fn);
/* Coalesce pause/resume/hold BT ops off net_mgmt / GATT / ISR contexts. */
static K_WORK_DEFINE(g_policy_work, policy_work_fn);

#define MT200_POL_PAUSE_BT  BIT(0)
#define MT200_POL_RESUME    BIT(1)
#define MT200_POL_HOLD      BIT(2)

static atomic_t g_policy_req;
static atomic_t g_hold_want;

static void policy_request(uint32_t bits)
{
	(void)atomic_or(&g_policy_req, (atomic_val_t)bits);
	(void)k_work_submit(&g_policy_work);
}

static void schedule_time_sync(uint32_t delay_ms)
{
	if (g_conn == NULL || g_write_handle == 0U || g_paused) {
		return;
	}
	(void)k_work_reschedule(&g_time_sync_work, K_MSEC(delay_ms));
}

static void time_sync_work_fn(struct k_work *work)
{
	/* No APP_ENTER here — fires every 5s until phone TIME lands and drowned the hang log. */
	ARG_UNUSED(work);

	if (g_conn == NULL || g_write_handle == 0U || g_paused) {
		return;
	}
	if (!clock_sync_is_synced()) {
		/* Keep polling until phone/NTP has set wall clock. */
		schedule_time_sync(5000U);
		return;
	}
	APP_ENTER();
	cmd_enqueue(MT200_CMD_SETTIME);
	schedule_time_sync(MT200_TIME_SYNC_MS);
	APP_LEAVE();
}

static void telem_set(uint8_t flag, uint8_t hr, uint8_t spo2, uint32_t steps, uint8_t bat)
{
	if ((flag & MT200_FLAG_HR) != 0U) {
		g_telem.hr = hr;
	}
	if ((flag & MT200_FLAG_SPO2) != 0U) {
		g_telem.spo2 = spo2;
	}
	if ((flag & MT200_FLAG_STEPS) != 0U) {
		g_telem.steps = steps;
	}
	if ((flag & MT200_FLAG_BAT) != 0U) {
		g_telem.bat_pct = bat;
	}
	g_telem.flags |= flag;
	g_telem.seq++;
	(void)mt200_sample_queue_offer_live(&g_telem, clock_sync_now_ms32());
	session_try_complete();
}

static int8_t rssi_sanitize(int8_t rssi)
{
	/* Core spec: 127 = RSSI not available. Map to worst dBm, not +127. */
	if (rssi == 127) {
		return MT200_RSSI_UNAVAIL;
	}
	return rssi;
}

static void read_conn_rssi(void)
{
	struct net_buf *buf;
	struct net_buf *rsp = NULL;
	struct bt_hci_cp_read_rssi *cp;
	struct bt_hci_rp_read_rssi *rp;
	uint16_t handle;
	int err;

	if (g_conn == NULL) {
		g_rssi = MT200_RSSI_UNAVAIL;
		return;
	}
	if (bt_hci_get_conn_handle(g_conn, &handle) != 0) {
		return;
	}

	/* Never block the BT WQ forever when phone + watch share HCI buffers. */
	buf = bt_hci_cmd_alloc(K_MSEC(50));
	if (buf == NULL) {
		return;
	}
	cp = net_buf_add(buf, sizeof(*cp));
	cp->handle = sys_cpu_to_le16(handle);
	err = bt_hci_cmd_send_sync(BT_HCI_OP_READ_RSSI, buf, &rsp);
	if (err || rsp == NULL) {
		/* Keep scan / last connected RSSI. Do not stomp with UNAVAIL on a
		 * transient HCI ENOMEM while the phone link is also busy. */
		return;
	}
	rp = (void *)rsp->data;
	if (rp->status == 0U) {
		g_rssi = rssi_sanitize(rp->rssi);
		LOG_DBG("MT200: conn rssi=%d", (int)g_rssi);
	}
	net_buf_unref(rsp);
}

static void schedule_restart(uint32_t delay_ms)
{
	if (!g_want_run) {
		return;
	}
	(void)k_work_reschedule(&g_restart_work, K_MSEC(delay_ms));
}

static void phase_set(uint8_t phase, uint32_t timeout_ms)
{
	g_phase = phase;
	g_mt200_phase = phase;
	g_phase_deadline_ms =
		(timeout_ms == 0U) ? 0U : (k_uptime_get_32() + timeout_ms);
	if (timeout_ms > 0U) {
		(void)k_work_reschedule(&g_sess_watch, K_MSEC(timeout_ms));
	} else {
		(void)k_work_cancel_delayable(&g_sess_watch);
	}
}

static bool mt200_radio_busy_phase(void)
{
	return g_phase == MT200_PHASE_SCAN || g_phase == MT200_PHASE_CONNECT ||
	       g_phase == MT200_PHASE_FETCH;
}

static void session_enter_quiet(const char *why)
{
	LOG_INF("MT200: quiet %umin (%s) — release phone adv + panel SPI (epoch free)",
		MT200_T_QUIET_MS / 60000U, why);
	phase_set(MT200_PHASE_QUIET, 0U);
	g_active = false;
	g_sess_ok = false;
	g_quiet_entered_ms = k_uptime_get_32();
	g_quiet_spi_done = false;
	g_mt200_quiet_spi_armed = 1U;
	/*
	 * Exclusive-epoch: hold peripheral adv + panel
	 * SPI only while the central session owns the radio. Quiet MUST
	 * re-advertise and redraw.
	 */
	radio_scheduler_panel_hold(false);
	ble_imu_set_mt200_adv_hold(false);
	renderer_spi_clear_trip(); /* allow cube after epoch; soft-trip is recoverable */
	g_mt200_busy = 0U;
	schedule_restart(MT200_T_QUIET_MS);
}

static void session_abort(const char *why)
{
	LOG_WRN("MT200: session abort (%s) — reschedule after quiet", why);
	(void)k_work_cancel_delayable(&g_poll_work);
	(void)bt_le_scan_stop();
	session_idle();
	if (g_conn != NULL) {
		g_sess_ok = false;
		(void)bt_conn_disconnect(g_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		/* disconnected() → session_enter_quiet */
	} else {
		session_enter_quiet(why);
	}
}

static void session_try_complete(void)
{
	if (g_phase != MT200_PHASE_FETCH) {
		return;
	}
	/* Sport frames arrive in a few seconds. Hold the link through the HR
	 * lock and the SpO2 window or those notifies never land. */
	if (!g_vitals_done) {
		return;
	}
	if ((g_telem.flags & MT200_SESS_NEED_ANY) == 0U) {
		return;
	}
	LOG_INF("MT200: session OK flags=0x%02x — disconnect for quiet", g_telem.flags);
	g_sess_ok = true;
	(void)k_work_cancel_delayable(&g_poll_work);
	(void)k_work_cancel_delayable(&g_sess_watch);
	if (g_conn != NULL) {
		(void)bt_conn_disconnect(g_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	} else {
		session_enter_quiet("ok-no-conn");
	}
}

static void sess_watch_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	const uint32_t now = k_uptime_get_32();

	if (g_phase_deadline_ms == 0U ||
	    (int32_t)(now - g_phase_deadline_ms) < 0) {
		return;
	}

	switch (g_phase) {
	case MT200_PHASE_SCAN:
		session_abort("scan timeout");
		break;
	case MT200_PHASE_CONNECT:
		session_abort("connect timeout");
		break;
	case MT200_PHASE_FETCH:
		session_abort("fetch timeout (data missed window)");
		break;
	default:
		break;
	}
}

static void start_write_discovery(struct bt_conn *conn)
{
	g_state = DISC_WRITE_CHRC;
	memset(&g_disc, 0, sizeof(g_disc));
	g_disc.func = discover_cb;
	g_disc.uuid = &write_uuid.uuid;
	g_disc.start_handle = 0x0001;
	g_disc.end_handle = g_svc_end_handle;
	g_disc.type = BT_GATT_DISCOVER_CHARACTERISTIC;

	const int err = bt_gatt_discover(conn, &g_disc);

	if (err) {
		LOG_WRN("MT200: write-char discover failed (err %d)", err);
	}
}

static uint8_t notify_cb(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
			  const void *data, uint16_t length)
{
//	APP_ENTER();
	ARG_UNUSED(conn);

	if (!data) {
		LOG_INF("MT200: unsubscribed");
		params->value_handle = 0U;
		APP_LEAVE();
		return BT_GATT_ITER_STOP;
	}

	const uint8_t *b = data;

	/* Password / device-function frame. Only treat as unlock after we sent A1. */
	if (length >= 6 && b[0] == (uint8_t)0xA1) {
		const uint32_t dev_n =
			((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 8) |
			(uint32_t)b[4];

		if (g_pwd_sent && !g_pwd_ok) {
			g_pwd_ok = true;
			LOG_INF("MT200: pwd ack A1 dev=%u — sport/HR unlocked", dev_n);
			/* Do not A5 here from uptime — wait for phone wall clock. */
			if (clock_sync_is_synced()) {
				cmd_enqueue(MT200_CMD_SETTIME);
				schedule_time_sync(MT200_TIME_SYNC_MS);
			} else {
				schedule_time_sync(5000U);
			}
		} else if (!g_pwd_sent) {
			LOG_INF("MT200: A1 before pwd-confirm (dev=%u) — ignoring", dev_n);
		}
//		APP_LEAVE();
		return BT_GATT_ITER_CONTINUE;
	}

	/* settingTime ack (vp_cv): [1] is response code. */
	if (length >= 2 && b[0] == (uint8_t)0xA5) {
		LOG_INF("MT200: time sync ack A5 status=%u", b[1]);
//		APP_LEAVE();
		return BT_GATT_ITER_CONTINUE;
	}

	if (length >= 2 && b[0] == (uint8_t)0xD0) {
		const uint8_t bpm = b[1];
		const uint8_t st = length >= 6 ? b[5] : 0xFFU;

		if (bpm >= 30U && bpm <= 220U) {
			telem_set(MT200_FLAG_HR, bpm, 0, 0, 0);
			LOG_DBG("MT200: heart-rate %u bpm (status=%u)", bpm, st);
		}
//		APP_LEAVE();
		return BT_GATT_ITER_CONTINUE;
	}

	/* SpO2: [3]==1 is unpass-wear, not 1%. Started only after the HR lock
	 * window, because this opcode takes the shared PPG. */
	if (length >= 5 && b[0] == (uint8_t)0x80) {
		const uint8_t spo2 = b[3];

		if (spo2 >= 70U && spo2 <= 100U) {
			telem_set(MT200_FLAG_SPO2, 0, spo2, 0, 0);
			LOG_DBG("MT200: spo2 %u%%", spo2);
		}
//		APP_LEAVE();
		return BT_GATT_ITER_CONTINUE;
	}

	if (length >= 6 && b[0] == (uint8_t)0xA8) {
		uint32_t steps = 0;

		if (b[5] == 0U) {
			steps = ((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16) |
				((uint32_t)b[3] << 8) | (uint32_t)b[4];
			if (steps == 0xFFFFFFFFU) {
				steps = 0U;
			}
			if ((g_telem.flags & MT200_FLAG_STEPS) == 0U || g_telem.steps != steps) {
				telem_set(MT200_FLAG_STEPS, 0, 0, steps, 0);
				LOG_DBG("MT200: steps(A8) %u", steps);
			}
		}
//		APP_LEAVE();
		return BT_GATT_ITER_CONTINUE;
	}

	if (length >= 6 && b[0] == (uint8_t)0xD8) {
		uint32_t steps = 0;
		uint32_t dist_mm = 0;
		uint32_t kcal_x10 = 0;
		bool changed = false;

		for (uint16_t i = 0; i < 4U && (2U + i) < length; i++) {
			steps |= ((uint32_t)b[2U + i]) << (8U * i);
		}
		for (uint16_t i = 0; i < 4U && (6U + i) < length; i++) {
			dist_mm |= ((uint32_t)b[6U + i]) << (8U * i);
		}
		for (uint16_t i = 0; i < 4U && (10U + i) < length; i++) {
			kcal_x10 |= ((uint32_t)b[10U + i]) << (8U * i);
		}

		if ((g_telem.flags & MT200_FLAG_STEPS) == 0U || g_telem.steps != steps) {
			g_telem.steps = steps;
			g_telem.flags |= MT200_FLAG_STEPS;
			changed = true;
		}
		if ((g_telem.flags & MT200_FLAG_DIST) == 0U || g_telem.dist_mm != dist_mm) {
			g_telem.dist_mm = dist_mm;
			g_telem.flags |= MT200_FLAG_DIST;
			changed = true;
		}
		if ((g_telem.flags & MT200_FLAG_KCAL) == 0U || g_telem.kcal_x10 != kcal_x10) {
			g_telem.kcal_x10 = kcal_x10;
			g_telem.flags |= MT200_FLAG_KCAL;
			changed = true;
		}
		if (changed) {
			g_telem.seq++;
			LOG_DBG("MT200: sport(D8) steps=%u dist_mm=%u kcal_x10=%u", steps, dist_mm,
				kcal_x10);
			(void)mt200_sample_queue_offer_live(&g_telem, clock_sync_now_ms32());
			session_try_complete();
		}
//		APP_LEAVE();
		return BT_GATT_ITER_CONTINUE;
	}

	if (length >= 8 && b[0] == (uint8_t)0xA0) {
		uint8_t pct = b[6];

		if (pct < 1U || pct > 100U) {
			pct = b[4];
		}
		if (pct >= 1U && pct <= 100U) {
			telem_set(MT200_FLAG_BAT, 0, 0, 0, pct);
			LOG_DBG("MT200: battery %u%%", pct);
		}
		APP_LEAVE();
		return BT_GATT_ITER_CONTINUE;
	}

	/* Rate-limited hex of everything else — needed to descramble unsolicited
	 * opcodes (BD status, A1/A5 time, etc.) without flooding the BT RX WQ. */
	if (length > 0U) {
		static uint8_t last_op;
		static uint32_t last_ms;
		static uint8_t dumps;
		const uint32_t now = k_uptime_get_32();

		if (dumps < 48U && (b[0] != last_op || (now - last_ms) >= 8000U)) {
			char hex[41];
			const uint16_t n = MIN(length, 20U);

			for (uint16_t i = 0; i < n; i++) {
				hex[i * 2U] = "0123456789abcdef"[b[i] >> 4];
				hex[i * 2U + 1U] = "0123456789abcdef"[b[i] & 0x0FU];
			}
			hex[n * 2U] = '\0';
			last_op = b[0];
			last_ms = now;
			dumps++;
			LOG_DBG("MT200: unk op=%02x n=%u %s", b[0], length, hex);
		}
	}

	APP_LEAVE();
	return BT_GATT_ITER_CONTINUE;
}

static void send_cmd(const uint8_t *prefix, size_t n, const char *label)
{
	uint8_t cmd[20];

	APP_ENTER();
	if (g_conn == NULL || g_write_handle == 0U) {
		LOG_WRN("MT200: %s skipped (no write handle)", label);
		APP_LEAVE();
		return;
	}

	memset(cmd, 0, sizeof(cmd));
	memcpy(cmd, prefix, MIN(n, sizeof(cmd)));

	LOG_DBG(">> gatt_write %s", label);
	const int err = bt_gatt_write_without_response(g_conn, g_write_handle, cmd, sizeof(cmd),
						       false);
	LOG_DBG("<< gatt_write %s err=%d", label, err);

	if (err) {
		LOG_WRN("MT200: %s write handle=%u err=%d", label, g_write_handle, err);
	} else {
		LOG_DBG("MT200: %s write handle=%u", label, g_write_handle);
	}
	APP_LEAVE();
}

static void cmd_clear(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_cmdq_lock);

	g_cmdq_n = 0U;
	g_cmdq_rd = 0U;
	k_spin_unlock(&g_cmdq_lock, key);
}

static void cmd_enqueue(uint8_t cmd)
{
	bool kick = false;

	if (cmd == 0U) {
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&g_cmdq_lock);

	if (g_cmdq_n >= MT200_CMDQ_CAP) {
		k_spin_unlock(&g_cmdq_lock, key);
		LOG_WRN("MT200: cmdq full, drop %u", cmd);
		return;
	}
	kick = (g_cmdq_n == 0U);
	g_cmdq[(g_cmdq_rd + g_cmdq_n) % MT200_CMDQ_CAP] = cmd;
	g_cmdq_n++;
	k_spin_unlock(&g_cmdq_lock, key);
	if (kick) {
		(void)k_work_reschedule(&g_cmd_work, K_NO_WAIT);
	}
}

static void session_idle(void)
{
	cmd_clear();
	g_spo2_busy = false;
	g_vitals_done = false;
	(void)k_work_cancel_delayable(&g_cmd_work);
	(void)k_work_cancel_delayable(&g_spo2_stop_work);
	(void)k_work_cancel_delayable(&g_vitals_spo2_work);
	(void)k_work_cancel_delayable(&g_poll_work);
	(void)k_work_cancel_delayable(&g_time_sync_work);
	(void)k_work_cancel_delayable(&g_sess_watch);
}

static void cmd_work_fn(struct k_work *work)
{

	APP_ENTER();
	ARG_UNUSED(work);

	static const uint8_t gsensor[] = { 0xF1, 0x20 };
	static const uint8_t hr_start[] = { 0xD0, 0x01 };
	static const uint8_t hr_stop[] = { 0xD0, 0x00 };
	static const uint8_t a8[] = { 0xA8, 0x00 };
	static const uint8_t d8[] = { 0xD8, 0x00 };
	static const uint8_t a0[] = { 0xA0, 0x00 };
	static const uint8_t spo2_start[] = { 0x80, 0x01, 0x02 };
	static const uint8_t spo2_stop[] = { 0x80, 0x02, 0x02 };
	uint8_t pwd_cmd[20];
	uint8_t time_cmd[20];

	if (g_conn == NULL || g_write_handle == 0U) {
		APP_LEAVE();
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&g_cmdq_lock);

	if (g_cmdq_n == 0U) {
		k_spin_unlock(&g_cmdq_lock, key);
		APP_LEAVE();
		return;
	}

	const uint8_t cmd = g_cmdq[g_cmdq_rd];

	g_cmdq_rd = (uint8_t)((g_cmdq_rd + 1U) % MT200_CMDQ_CAP);
	g_cmdq_n--;

	const bool more = (g_cmdq_n > 0U);

	k_spin_unlock(&g_cmdq_lock, key);

	switch (cmd) {
	case MT200_CMD_PWD:
		build_pwd_cmd(pwd_cmd);
		g_pwd_sent = true;
		send_cmd(pwd_cmd, sizeof(pwd_cmd), "pwd-confirm(A1)");
		break;
	case MT200_CMD_SETTIME:
		build_settime_cmd(time_cmd);
		send_cmd(time_cmd, sizeof(time_cmd), "time-sync(A5)");
		break;
	case MT200_CMD_GSENSOR:
		send_cmd(gsensor, sizeof(gsensor), "gsensor-start(F1 20)");
		break;
	case MT200_CMD_HR_START:
		send_cmd(hr_start, sizeof(hr_start), "HR-start");
		g_spo2_busy = false;
		break;
	case MT200_CMD_HR_STOP:
		send_cmd(hr_stop, sizeof(hr_stop), "HR-stop");
		break;
	case MT200_CMD_A8:
		send_cmd(a8, sizeof(a8), "steps-read(A8)");
		break;
	case MT200_CMD_D8:
		send_cmd(d8, sizeof(d8), "sport-read(D8)");
		break;
	case MT200_CMD_A0:
		send_cmd(a0, sizeof(a0), "battery-read(A0)");
		break;
	case MT200_CMD_SPO2_START:
		send_cmd(spo2_start, sizeof(spo2_start), "SpO2-start");
		g_spo2_busy = true;
		(void)k_work_reschedule(&g_spo2_stop_work, K_MSEC(MT200_SPO2_HOLD_MS));
		break;
	case MT200_CMD_SPO2_STOP:
		send_cmd(spo2_stop, sizeof(spo2_stop), "SpO2-stop");
		g_spo2_busy = false;
		g_vitals_done = true;
		session_try_complete();
		break;
	default:
		break;
	}

	if (more) {
		(void)k_work_reschedule(&g_cmd_work, K_MSEC(MT200_CMD_GAP_MS));
	}
	APP_LEAVE();
}

static void spo2_stop_work_fn(struct k_work *work)
{

	APP_ENTER();
	ARG_UNUSED(work);

	if (g_conn == NULL || g_write_handle == 0U) {
		g_spo2_busy = false;
		APP_LEAVE();
		return;
	}
	cmd_enqueue(MT200_CMD_SPO2_STOP);
	APP_LEAVE();
}

static void vitals_spo2_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (g_conn == NULL || g_write_handle == 0U || g_phase != MT200_PHASE_FETCH) {
		return;
	}
	cmd_enqueue(MT200_CMD_HR_STOP);
	cmd_enqueue(MT200_CMD_SPO2_START);
}

static void poll_work_fn(struct k_work *work)
{
	STACK_RA_CHECK_SETUP;
	APP_ENTER();
	ARG_UNUSED(work);

	if (g_conn == NULL || g_write_handle == 0U || g_phase != MT200_PHASE_FETCH) {
		g_rssi = MT200_RSSI_UNAVAIL;
		APP_LEAVE();
		return;
	}

	g_poll_tick++;

	/*
	 * One lite poll burst inside the FETCH window. If telem still missing,
	 * sess_watch aborts and reschedules; do not keep the link up indefinitely.
	 */
	/* A8/D8 during SpO2 steal the notify stream from the PPG frame. */
	if (!g_spo2_busy) {
		cmd_enqueue(MT200_CMD_A8);
		cmd_enqueue(MT200_CMD_D8);
	}
	cmd_enqueue(MT200_CMD_A0);
	read_conn_rssi();
	STACK_RA_CHECK();
	if (g_poll_tick < 3U) {
		(void)k_work_schedule(&g_poll_work, K_MSEC(4000));
	}
	APP_LEAVE();
}

static uint8_t discover_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			    struct bt_gatt_discover_params *params)
{

	APP_ENTER();
	int err;

	if (!attr) {
		LOG_INF("MT200: discover phase %d complete (no match)", g_state);
		memset(params, 0, sizeof(*params));
		APP_LEAVE();
		return BT_GATT_ITER_STOP;
	}

	switch (g_state) {
	case DISC_PRIMARY: {
		const struct bt_gatt_service_val *svc = attr->user_data;

		g_svc_end_handle = svc->end_handle;
		LOG_INF("MT200: service F0080001 found handle=%u end=%u", attr->handle,
			g_svc_end_handle);

		g_state = DISC_NOTIFY_CHRC;
		g_disc.uuid = &notify_uuid.uuid;
		g_disc.start_handle = attr->handle + 1;
		g_disc.end_handle = g_svc_end_handle;
		g_disc.type = BT_GATT_DISCOVER_CHARACTERISTIC;

		err = bt_gatt_discover(conn, &g_disc);
		if (err) {
			LOG_WRN("MT200: notify-char discover failed (err %d)", err);
		}
		APP_LEAVE();
		return BT_GATT_ITER_STOP;
	}
	case DISC_NOTIFY_CHRC: {
		const struct bt_gatt_chrc *chrc = attr->user_data;

		LOG_INF("MT200: notify char decl=%u value_handle=%u", attr->handle,
			chrc->value_handle);

		/* MT200's minimal GATT server appears not to implement the ATT "Find
		 * Information" opcode used by BT_GATT_DISCOVER_DESCRIPTOR — CCC
		 * discovery always silently comes back empty even though a direct
		 * read of the handle succeeds. A live GATT scan of this exact device
		 * confirmed its CCC always sits at value_handle + 1, so subscribe
		 * directly against that hardcoded offset instead of discovering it. */
		memset(&g_sub, 0, sizeof(g_sub));
		g_sub.value_handle = chrc->value_handle;
		g_sub.ccc_handle = chrc->value_handle + 1;
		g_sub.notify = notify_cb;
		g_sub.value = BT_GATT_CCC_NOTIFY;

		err = bt_gatt_subscribe(conn, &g_sub);
		if (err && err != -EALREADY) {
			LOG_WRN("MT200: subscribe (handle=%u) failed (err %d)", g_sub.ccc_handle,
				err);
		} else {
			LOG_INF("MT200: subscribed to notify via handle=%u", g_sub.ccc_handle);
		}

		start_write_discovery(conn);
		APP_LEAVE();
		return BT_GATT_ITER_STOP;
	}
	case DISC_WRITE_CHRC: {
		const struct bt_gatt_chrc *chrc = attr->user_data;

		g_write_handle = chrc->value_handle;
		LOG_INF("MT200: write char decl=%u value_handle=%u", attr->handle,
			g_write_handle);
		g_poll_tick = 0U;
		g_pwd_ok = false;
		g_pwd_sent = false;
		/* Must unlock with A1+PIN before sport/HR opcodes land.
		 * HR runs for MT200_HR_LOCK_MS, then one SpO2 window, then quiet. */
		cmd_enqueue(MT200_CMD_PWD);
		cmd_enqueue(MT200_CMD_GSENSOR);
		cmd_enqueue(MT200_CMD_HR_START);
		(void)k_work_reschedule(&g_vitals_spo2_work, K_MSEC(MT200_HR_LOCK_MS));
		(void)k_work_reschedule(&g_poll_work, K_MSEC(4000));
		if (clock_sync_is_synced()) {
			cmd_enqueue(MT200_CMD_SETTIME);
			schedule_time_sync(MT200_TIME_SYNC_MS);
		} else {
			schedule_time_sync(5000U);
		}
		APP_LEAVE();
		return BT_GATT_ITER_STOP;
	}
	default:
		APP_LEAVE();
		return BT_GATT_ITER_STOP;
	}
}

static void start_service_discovery(struct bt_conn *conn);

static void mt200_mtu_exchange_cb(struct bt_conn *conn, uint8_t err,
				  struct bt_gatt_exchange_params *params)
{
	ARG_UNUSED(params);
	if (err) {
		LOG_WRN("MT200: ATT MTU exchange failed (%u)", err);
	} else {
		LOG_INF("MT200: ATT MTU %u", bt_gatt_get_mtu(conn));
	}
}

static void start_service_discovery(struct bt_conn *conn)
{
	g_state = DISC_PRIMARY;
	memset(&g_disc, 0, sizeof(g_disc));
	g_disc.uuid = &svc_uuid.uuid;
	g_disc.func = discover_cb;
	g_disc.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	g_disc.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	g_disc.type = BT_GATT_DISCOVER_PRIMARY;

	const int err = bt_gatt_discover(conn, &g_disc);

	if (err) {
		LOG_WRN("MT200: primary-service discover failed (err %d)", err);
	}
}

static bool eir_found(struct bt_data *data, void *user_data)
{
	ARG_UNUSED(data);
	ARG_UNUSED(user_data);
	return true;
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			  struct net_buf_simple *ad)
{
	char dev[BT_ADDR_LE_STR_LEN];
	bt_addr_le_t target;

	bt_addr_le_to_str(addr, dev, sizeof(dev));

	if (bt_addr_le_from_str(MT200_ADDR_STR, "public", &target) != 0) {
		return;
	}
	if (bt_addr_le_cmp(addr, &target) != 0) {
		return;
	}

	APP_ENTER();
	LOG_INF("MT200: found %s rssi=%d type=%u", dev, rssi, type);
	g_rssi = rssi_sanitize(rssi);
	(void)bt_data_parse(ad, eir_found, NULL);

	if (bt_le_scan_stop() != 0) {
		APP_LEAVE();
		return;
	}

	phase_set(MT200_PHASE_CONNECT, MT200_T_CONN_MS);

	const struct bt_le_conn_param *param = BT_LE_CONN_PARAM_DEFAULT;
	struct bt_conn *new_conn = NULL;
	const int err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, param, &new_conn);

	if (err) {
		LOG_WRN("MT200: connect create failed (err %d)", err);
		g_active = false;
		session_abort("connect create failed");
	} else {
		g_conn = new_conn;
	}
	APP_LEAVE();
}

static void connected(struct bt_conn *conn, uint8_t conn_err)
{

	APP_ENTER();
	bt_addr_le_t target;

	if (bt_addr_le_from_str(MT200_ADDR_STR, "public", &target) != 0) {
		APP_LEAVE();
		return;
	}
	if (bt_addr_le_cmp(bt_conn_get_dst(conn), &target) != 0) {
		APP_LEAVE();
		return;
	}

	if (conn_err) {
		LOG_WRN("MT200: connect failed (err 0x%02x)", conn_err);
		bt_conn_unref(g_conn);
		g_conn = NULL;
		g_rssi = MT200_RSSI_UNAVAIL;
		g_active = false;
		session_abort("connect failed");
		APP_LEAVE();
		return;
	}

	LOG_INF("MT200: connected — MTU/DLE + GATT discovery");
	g_link_up_ms = k_uptime_get_32();
	phase_set(MT200_PHASE_FETCH, MT200_T_FETCH_MS);

#if defined(CONFIG_BT_USER_DATA_LEN_UPDATE)
	{
		const struct bt_conn_le_data_len_param *dle = BT_LE_DATA_LEN_PARAM_MAX;
		const int dle_err = bt_conn_le_data_len_update(conn, dle);

		if (dle_err != 0 && dle_err != -EALREADY) {
			LOG_WRN("MT200: DLE update failed (%d)", dle_err);
		}
	}
#endif
	{
		static struct bt_gatt_exchange_params mtu_ex;

		mtu_ex.func = mt200_mtu_exchange_cb;
		const int mtu_err = bt_gatt_exchange_mtu(conn, &mtu_ex);

		if (mtu_err != 0 && mtu_err != -EALREADY) {
			LOG_WRN("MT200: ATT MTU exchange start failed (%d)", mtu_err);
		}
	}

	start_service_discovery(conn);
	APP_LEAVE();
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{

	APP_ENTER();
	bt_addr_le_t target;

	if (bt_addr_le_from_str(MT200_ADDR_STR, "public", &target) != 0) {
		APP_LEAVE();
		return;
	}
	if (bt_addr_le_cmp(bt_conn_get_dst(conn), &target) != 0) {
		APP_LEAVE();
		return;
	}

	LOG_INF("MT200: disconnected (reason 0x%02x)", reason);
	bt_conn_unref(g_conn);
	g_conn = NULL;
	g_write_handle = 0U;
	g_pwd_ok = false;
	g_pwd_sent = false;
	g_rssi = MT200_RSSI_UNAVAIL;
	g_active = false;
	g_link_up_ms = 0U;
	session_idle();
	if (g_sess_ok) {
		g_sess_ok = false;
		session_enter_quiet("session ok");
	} else if (g_phase != MT200_PHASE_QUIET) {
		session_enter_quiet("link drop");
	} else {
		schedule_restart(MT200_RESTART_MS);
	}
	APP_LEAVE();
}

BT_CONN_CB_DEFINE(mt200_conn_cb) = {
	.connected = connected,
	.disconnected = disconnected,
};

bool mt200_bridge_radio_busy(void)
{
	const bool busy = mt200_radio_busy_phase() || g_active || (g_conn != NULL);

	g_mt200_busy = busy ? 1U : 0U;
	return busy;
}

bool mt200_bridge_quiet_spi_ok(void)
{
	if (g_phase != MT200_PHASE_QUIET || !g_want_run || g_quiet_spi_done) {
		g_mt200_quiet_spi_armed = 0U;
		return false;
	}
	const uint32_t now = k_uptime_get_32();

	if ((int32_t)(now - g_quiet_entered_ms) < (int32_t)MT200_QUIET_SPI_SETTLE_MS) {
		return false;
	}
	g_mt200_quiet_spi_armed = 1U;
	return true;
}

void mt200_bridge_quiet_spi_consumed(void)
{
	g_quiet_spi_done = true;
	g_mt200_quiet_spi_armed = 0U;
}

bool mt200_bridge_wanted(void)
{
	g_mt200_want = g_want_run ? 1U : 0U;
	return g_want_run;
}

bool mt200_bridge_active(void)
{
	/* SPI / flash-erase gate: only while a session owns the radio. */
	return mt200_bridge_radio_busy();
}

void mt200_bridge_telem(struct mt200_telem *out)
{
	if (out == NULL) {
		return;
	}
	*out = g_telem;
	out->rssi = g_rssi;
}

void mt200_bridge_sync_time(void)
{

	APP_ENTER();
	if (g_paused || g_conn == NULL || g_write_handle == 0U) {
		APP_LEAVE();
		return;
	}
	if (!clock_sync_is_synced()) {
		LOG_DBG("MT200: time sync skipped (ESP has no phone/NTP wall clock yet)");
		schedule_time_sync(5000U);
		APP_LEAVE();
		return;
	}
	cmd_enqueue(MT200_CMD_SETTIME);
	schedule_time_sync(MT200_TIME_SYNC_MS);
	APP_LEAVE();
}

void mt200_bridge_start(void)
{

	APP_ENTER();
	if (g_paused || g_phone_hold) {
		LOG_INF("MT200: start ignored (%s)",
			g_phone_hold ? "phone hold" : "paused for OTA/WiFi");
		APP_LEAVE();
		return;
	}
	g_want_run = true;
	(void)k_work_cancel_delayable(&g_restart_work);

	if (mt200_bridge_radio_busy()) {
		LOG_WRN("MT200: bridge already busy — ignoring start request");
		APP_LEAVE();
		return;
	}

	/* Exclusive epoch: wait out panel flush, then hold SPI + phone adv. */
	for (int i = 0; i < 80; i++) {
		if (!renderer_busy()) {
			break;
		}
		k_msleep(10);
	}
	radio_scheduler_panel_hold(true);
	ble_imu_set_mt200_adv_hold(true);
	g_quiet_spi_done = true; /* no panel SPI during session */
	g_mt200_quiet_spi_armed = 0U;

	g_active = true;
	g_write_handle = 0U;
	g_sess_ok = false;
	g_poll_tick = 0U;
	phase_set(MT200_PHASE_SCAN, MT200_T_SCAN_MS);

	static const struct bt_le_scan_param scan_param = {
		.type = BT_LE_SCAN_TYPE_ACTIVE,
		.options = BT_LE_SCAN_OPT_NONE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window = BT_GAP_SCAN_FAST_WINDOW,
	};

	const int err = bt_le_scan_start(&scan_param, device_found);

	if (err) {
		LOG_WRN("MT200: scan start failed (err %d) — MT200 may already be linked "
			"to its phone app (single-LE-link device)",
			err);
		g_active = false;
		radio_scheduler_panel_hold(false);
		session_enter_quiet("scan start failed");
		APP_LEAVE();
		return;
	}

	LOG_INF("MT200: session scan for " MT200_ADDR_STR " (≤%us)",
		MT200_T_SCAN_MS / 1000U);
	APP_LEAVE();
}

void mt200_bridge_autostart(void)
{

	APP_ENTER();
	mt200_sample_queue_init();
	if (g_paused || g_phone_hold) {
		LOG_INF("MT200: autostart deferred (%s)",
			g_phone_hold ? "phone hold" : "paused");
		APP_LEAVE();
		return;
	}
	g_want_run = true;
	g_mt200_want = 1U;
	LOG_INF("MT200: autostart in %us (adv held only during live scan/connect/fetch)",
		MT200_AUTOSTART_MS / 1000U);
	/* Leave phone advertising up until the first session actually starts. */
	ble_imu_set_mt200_adv_hold(false);
	(void)k_work_reschedule(&g_restart_work, K_MSEC(MT200_AUTOSTART_MS));
	APP_LEAVE();
}

void mt200_bridge_pause(void)
{
	/* Flags sync so start()/restart skip immediately; BT teardown is queued. */
	g_paused = true;
	g_want_run = false;
	radio_scheduler_panel_hold(false);
	ble_imu_set_mt200_adv_hold(false);
	(void)k_work_cancel_delayable(&g_restart_work);
	policy_request(MT200_POL_PAUSE_BT);
}

void mt200_bridge_resume(void)
{
	g_paused = false;
	if (g_phone_hold) {
		g_want_run = true;
		policy_request(MT200_POL_RESUME); /* log + no-op restart under hold */
		return;
	}
	if (g_want_run) {
		return;
	}
	g_want_run = true;
	policy_request(MT200_POL_RESUME);
}

void mt200_bridge_set_phone_hold(bool hold)
{
	atomic_set(&g_hold_want, hold ? 1 : 0);
	policy_request(MT200_POL_HOLD);
}

static void policy_apply_pause_bt(void)
{
	APP_ENTER();
	(void)k_work_cancel_delayable(&g_restart_work);
	session_idle();
	(void)bt_le_scan_stop();
	g_active = false;
	if (g_conn != NULL) {
		LOG_INF("MT200: pausing for WiFi radio");
		(void)bt_conn_disconnect(g_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
	APP_LEAVE();
}

static void policy_apply_resume(void)
{
	APP_ENTER();
	if (g_phone_hold) {
		LOG_INF("MT200: resume deferred (phone hold)");
		APP_LEAVE();
		return;
	}
	if (!g_want_run || g_paused) {
		APP_LEAVE();
		return;
	}
	LOG_INF("MT200: resume after WiFi");
	schedule_restart(8000U);
	APP_LEAVE();
}

static void policy_apply_hold(bool hold)
{
	APP_ENTER();
	if (hold == g_phone_hold) {
		APP_LEAVE();
		return;
	}
	g_phone_hold = hold;
	if (hold) {
		LOG_INF("MT200: phone hold — dropping central, phone adv free");
		(void)k_work_cancel_delayable(&g_restart_work);
		session_idle();
		(void)bt_le_scan_stop();
		g_active = false;
		g_want_run = true;
		/* Phone link owns BLE — never keep MT200 adv/SPI hold latched. */
		radio_scheduler_panel_hold(false);
		ble_imu_set_mt200_adv_hold(false);
		if (g_conn != NULL) {
			(void)bt_conn_disconnect(g_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		}
	} else {
		LOG_INF("MT200: phone hold clear");
		if (!g_paused && g_want_run) {
			schedule_restart(8000U);
		}
	}
	APP_LEAVE();
}

static void policy_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	/*
	 * Drain coalesced requests on the system workqueue (thread context).
	 * Order: hold first (radio ownership), then pause BT, then resume.
	 * Re-read atomics each pass so a new submit during apply is not lost.
	 */
	for (int pass = 0; pass < 4; pass++) {
		const atomic_val_t req = atomic_set(&g_policy_req, 0);

		if (req == 0) {
			break;
		}
		if ((req & MT200_POL_HOLD) != 0) {
			policy_apply_hold(atomic_get(&g_hold_want) != 0);
		}
		if ((req & MT200_POL_PAUSE_BT) != 0) {
			policy_apply_pause_bt();
		}
		if ((req & MT200_POL_RESUME) != 0) {
			policy_apply_resume();
		}
	}
}

static void restart_work_fn(struct k_work *work)
{

	APP_ENTER();
	ARG_UNUSED(work);
	if (g_phone_hold || g_paused) {
		LOG_INF("MT200: restart skipped (%s)",
			g_phone_hold ? "phone hold" : "paused");
		APP_LEAVE();
		return;
	}
	g_active = false;
	mt200_bridge_start();
	APP_LEAVE();
}

#else /* !CONFIG_APP_CRASH_DEBUG */

bool mt200_bridge_active(void)
{
	return false;
}

bool mt200_bridge_radio_busy(void)
{
	return false;
}

bool mt200_bridge_quiet_spi_ok(void)
{
	return false;
}

void mt200_bridge_quiet_spi_consumed(void)
{
}

bool mt200_bridge_wanted(void)
{
	return false;
}

void mt200_bridge_start(void)
{
	LOG_WRN("MT200: bridge unavailable (CONFIG_APP_CRASH_DEBUG=n)");
}

void mt200_bridge_autostart(void)
{
}

void mt200_bridge_pause(void)
{
}

void mt200_bridge_resume(void)
{
}

void mt200_bridge_set_phone_hold(bool hold)
{
	ARG_UNUSED(hold);
}

void mt200_bridge_telem(struct mt200_telem *out)
{
	if (out == NULL) {
		return;
	}
	memset(out, 0, sizeof(*out));
	out->rssi = MT200_RSSI_UNAVAIL;
}

void mt200_bridge_sync_time(void)
{
}

#endif /* CONFIG_APP_CRASH_DEBUG */
