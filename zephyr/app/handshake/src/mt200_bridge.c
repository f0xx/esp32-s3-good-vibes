#include "mt200_bridge.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(mt200, LOG_LEVEL_INF);

#if defined(CONFIG_APP_CRASH_DEBUG)

#define MT200_ADDR_STR "25:38:22:92:C9:4E"
#define MT200_AUTOSTART_MS 8000U
#define MT200_RESTART_MS 5000U
#define MT200_POLL_MS 10000U

/* clang-format off */
static struct bt_uuid_128 svc_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xf0080001, 0x0451, 0x4000, 0xb000, 0x000000000000));
static struct bt_uuid_128 notify_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xf0080002, 0x0451, 0x4000, 0xb000, 0x000000000000));
static struct bt_uuid_128 write_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xf0080003, 0x0451, 0x4000, 0xb000, 0x000000000000));
/* clang-format on */

enum disc_state {
	DISC_IDLE = 0,
	DISC_PRIMARY,
	DISC_NOTIFY_CHRC,
	DISC_WRITE_CHRC,
};

static struct bt_conn *g_conn;
static struct bt_gatt_discover_params g_disc;
static struct bt_gatt_subscribe_params g_sub;
static enum disc_state g_state;
static uint16_t g_svc_end_handle;
static uint16_t g_write_handle;
static bool g_active;
static bool g_want_run;
static struct mt200_telem g_telem;
static uint8_t g_poll_tick;
static int8_t g_rssi = MT200_RSSI_UNAVAIL;

static uint8_t discover_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			    struct bt_gatt_discover_params *params);
static void poll_work_fn(struct k_work *work);
static void restart_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_poll_work, poll_work_fn);
static K_WORK_DELAYABLE_DEFINE(g_restart_work, restart_work_fn);

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

	buf = bt_hci_cmd_create(BT_HCI_OP_READ_RSSI, sizeof(*cp));
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
	ARG_UNUSED(conn);

	if (!data) {
		LOG_INF("MT200: unsubscribed");
		params->value_handle = 0U;
		return BT_GATT_ITER_STOP;
	}

	const uint8_t *b = data;

	if (length >= 2 && b[0] == (uint8_t)0xD0) {
		const uint8_t bpm = b[1];
		const uint8_t st = length >= 6 ? b[5] : 0xFFU;

		if (bpm >= 30U && bpm <= 220U) {
			telem_set(MT200_FLAG_HR, bpm, 0, 0, 0);
			LOG_DBG("MT200: heart-rate %u bpm (status=%u)", bpm, st);
		}
		return BT_GATT_ITER_CONTINUE;
	}

	/* SpO2: [3]==1 is unpass-wear, not 1%. Do not auto-start this opcode — it
	 * steals the shared PPG from HR. Keep the decoder so a later sample lands. */
	if (length >= 5 && b[0] == (uint8_t)0x80) {
		const uint8_t spo2 = b[3];

		if (spo2 >= 70U && spo2 <= 100U) {
			telem_set(MT200_FLAG_SPO2, 0, spo2, 0, 0);
			LOG_DBG("MT200: spo2 %u%%", spo2);
		}
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
		return BT_GATT_ITER_CONTINUE;
	}

	if (length >= 14 && b[0] == (uint8_t)0xD8) {
		const uint32_t steps = ((uint32_t)b[5] << 24) | ((uint32_t)b[4] << 16) |
				       ((uint32_t)b[3] << 8) | (uint32_t)b[2];

		if ((g_telem.flags & MT200_FLAG_STEPS) == 0U || g_telem.steps != steps) {
			telem_set(MT200_FLAG_STEPS, 0, 0, steps, 0);
			LOG_DBG("MT200: steps(D8) %u", steps);
		}
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
		return BT_GATT_ITER_CONTINUE;
	}

	/* Rate-limited hex of everything else — needed to descramble unsolicited
	 * opcodes (BD status, A1/A5 time, etc.) without flooding the BT RX WQ. */
	{
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
			LOG_INF("MT200: unk op=%02x n=%u %s", b[0], length, hex);
		}
	}

	return BT_GATT_ITER_CONTINUE;
}

static void send_cmd(const uint8_t *prefix, size_t n, const char *label)
{
	uint8_t cmd[20];

	if (g_conn == NULL || g_write_handle == 0U) {
		LOG_WRN("MT200: %s skipped (no write handle)", label);
		return;
	}

	memset(cmd, 0, sizeof(cmd));
	memcpy(cmd, prefix, MIN(n, sizeof(cmd)));

	const int err = bt_gatt_write_without_response(g_conn, g_write_handle, cmd, sizeof(cmd),
							false);

	if (err) {
		LOG_WRN("MT200: %s write handle=%u err=%d", label, g_write_handle, err);
	} else {
		LOG_DBG("MT200: %s write handle=%u", label, g_write_handle);
	}
}

static void poll_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	static const uint8_t steps[] = { 0xA8, 0x00 };
	static const uint8_t batt[] = { 0xA0, 0x00 };

	if (g_conn == NULL || g_write_handle == 0U) {
		g_rssi = MT200_RSSI_UNAVAIL;
		return;
	}

	send_cmd(steps, sizeof(steps), "steps-read(A8)");
	if ((g_poll_tick++ % 3U) == 0U) {
		send_cmd(batt, sizeof(batt), "battery-read(A0)");
		/* HCI Read RSSI after ATT writes, and only on the slow tick — send_sync
		 * on the same moment as A8/A0 stole HCI buffers from the phone link. */
		read_conn_rssi();
	}
	(void)k_work_schedule(&g_poll_work, K_MSEC(MT200_POLL_MS));
}

static uint8_t discover_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			    struct bt_gatt_discover_params *params)
{
	int err;

	if (!attr) {
		LOG_INF("MT200: discover phase %d complete (no match)", g_state);
		memset(params, 0, sizeof(*params));
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
		return BT_GATT_ITER_STOP;
	}
	case DISC_WRITE_CHRC: {
		const struct bt_gatt_chrc *chrc = attr->user_data;

		g_write_handle = chrc->value_handle;
		LOG_INF("MT200: write char decl=%u value_handle=%u", attr->handle,
			g_write_handle);
		static const uint8_t hr[] = { 0xD0, 0x01 };

		send_cmd(hr, sizeof(hr), "HR-start");
		g_poll_tick = 0U;
		(void)k_work_reschedule(&g_poll_work, K_MSEC(4000));
		return BT_GATT_ITER_STOP;
	}
	default:
		return BT_GATT_ITER_STOP;
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

	LOG_INF("MT200: found %s rssi=%d type=%u", dev, rssi, type);
	g_rssi = rssi_sanitize(rssi);
	(void)bt_data_parse(ad, eir_found, NULL);

	if (bt_le_scan_stop() != 0) {
		return;
	}

	const struct bt_le_conn_param *param = BT_LE_CONN_PARAM_DEFAULT;
	const int err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, param, &g_conn);

	if (err) {
		LOG_WRN("MT200: connect create failed (err %d)", err);
		g_active = false;
		schedule_restart(MT200_RESTART_MS);
	}
}

static void connected(struct bt_conn *conn, uint8_t conn_err)
{
	bt_addr_le_t target;

	if (bt_addr_le_from_str(MT200_ADDR_STR, "public", &target) != 0) {
		return;
	}
	if (bt_addr_le_cmp(bt_conn_get_dst(conn), &target) != 0) {
		return;
	}

	if (conn_err) {
		LOG_WRN("MT200: connect failed (err 0x%02x)", conn_err);
		bt_conn_unref(g_conn);
		g_conn = NULL;
		g_rssi = MT200_RSSI_UNAVAIL;
		g_active = false;
		schedule_restart(MT200_RESTART_MS);
		return;
	}

	LOG_INF("MT200: connected — starting GATT discovery");
	start_service_discovery(conn);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	bt_addr_le_t target;

	if (bt_addr_le_from_str(MT200_ADDR_STR, "public", &target) != 0) {
		return;
	}
	if (bt_addr_le_cmp(bt_conn_get_dst(conn), &target) != 0) {
		return;
	}

	LOG_INF("MT200: disconnected (reason 0x%02x)", reason);
	bt_conn_unref(g_conn);
	g_conn = NULL;
	g_write_handle = 0U;
	g_rssi = MT200_RSSI_UNAVAIL;
	g_active = false;
	(void)k_work_cancel_delayable(&g_poll_work);
	schedule_restart(MT200_RESTART_MS);
}

BT_CONN_CB_DEFINE(mt200_conn_cb) = {
	.connected = connected,
	.disconnected = disconnected,
};

bool mt200_bridge_active(void)
{
	return g_active;
}

void mt200_bridge_telem(struct mt200_telem *out)
{
	if (out == NULL) {
		return;
	}
	*out = g_telem;
	out->rssi = g_rssi;
}

void mt200_bridge_start(void)
{
	g_want_run = true;
	(void)k_work_cancel_delayable(&g_restart_work);

	if (g_active) {
		LOG_WRN("MT200: bridge already active — ignoring start request");
		return;
	}

	g_active = true;
	g_write_handle = 0U;

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
		schedule_restart(MT200_RESTART_MS);
		return;
	}

	LOG_INF("MT200: scanning for " MT200_ADDR_STR);
}

void mt200_bridge_autostart(void)
{
	g_want_run = true;
	LOG_INF("MT200: autostart in %us", MT200_AUTOSTART_MS / 1000U);
	(void)k_work_reschedule(&g_restart_work, K_MSEC(MT200_AUTOSTART_MS));
}

static void restart_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	g_active = false;
	mt200_bridge_start();
}

#else /* !CONFIG_APP_CRASH_DEBUG */

bool mt200_bridge_active(void)
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

void mt200_bridge_telem(struct mt200_telem *out)
{
	if (out == NULL) {
		return;
	}
	memset(out, 0, sizeof(*out));
	out->rssi = MT200_RSSI_UNAVAIL;
}

#endif /* CONFIG_APP_CRASH_DEBUG */
