/*
 * ESP32 VHCI HCI driver — defer all VHCI RX off btController.
 *
 * btController (ESP-IDF BTC task) must not call ANY Zephyr API. Copy raw H4
 * bytes into a lock-free SPSC ring (memcpy + atomics only); a Zephyr RX thread
 * parses and bt_recv(). TX uses atomic ready flag from BTC + polling.
 *
 * The RX wait loop and BTC callbacks live in IRAM. Zephyr already links
 * libkernel (k_msleep) into IRAM; this driver's .flash.text used to run
 * immediately after that return. On ESP32-S3 + octal PSRAM the flash/PSRAM
 * MSPI is shared, so an instruction fetch of that memw/atomic_get can come
 * back as garbage → EXCCAUSE 0 (illegal instruction) at a perfectly valid
 * opcode. IRAM I-fetch does not use that bus. Literal pools must follow the
 * function (-mtext-section-literals, applied at flash time).
 */

#include <zephyr/bluetooth/hci.h>

#include <zephyr/init.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

#include <zephyr/drivers/bluetooth.h>

#include <esp_attr.h>
#include <esp_bt.h>

#define LOG_LEVEL CONFIG_BT_HCI_DRIVER_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(bt_hci_driver_esp32);

#define DT_DRV_COMPAT espressif_esp32_bt_hci
#define HCI_RAW_MAX         260
#define HCI_RAW_Q_DEPTH     10

struct bt_esp32_data {
	bt_hci_recv_t recv;
};

struct hci_raw_item {
	uint16_t len;
	uint8_t data[HCI_RAW_MAX];
};

#define HCI_SEND_POLL_MS     1
#define HCI_SEND_TIMEOUT_MS  2000

static DRAM_ATTR struct hci_raw_item hci_raw_ring[HCI_RAW_Q_DEPTH];
static DRAM_ATTR volatile uint32_t hci_ring_head_v;
static DRAM_ATTR volatile uint32_t hci_ring_tail_v;
static DRAM_ATTR volatile uint32_t hci_ring_drops_v;
static K_THREAD_STACK_DEFINE(hci_rx_stack, CONFIG_BT_RX_STACK_SIZE);
static struct k_thread hci_rx_thread;
static DRAM_ATTR atomic_t hci_rx_running;

static bool is_hci_event_discardable(const uint8_t *evt_data)
{
	uint8_t evt_type = evt_data[0];

	switch (evt_type) {
#if defined(CONFIG_BT_CLASSIC)
	case BT_HCI_EVT_INQUIRY_RESULT_WITH_RSSI:
	case BT_HCI_EVT_EXTENDED_INQUIRY_RESULT:
		return true;
#endif
	case BT_HCI_EVT_LE_META_EVENT: {
		uint8_t subevt_type = evt_data[sizeof(struct bt_hci_evt_hdr)];

		switch (subevt_type) {
		case BT_HCI_EVT_LE_ADVERTISING_REPORT:
			return true;
		default:
			return false;
		}
	}
	default:
		return false;
	}
}

static struct net_buf *bt_esp_evt_recv(uint8_t *data, size_t remaining)
{
	bool discardable = false;
	struct bt_hci_evt_hdr hdr;
	struct net_buf *buf;
	size_t buf_tailroom;

	if (remaining < sizeof(hdr)) {
		LOG_ERR("Not enough data for event header");
		return NULL;
	}

	discardable = is_hci_event_discardable(data);

	memcpy((void *)&hdr, data, sizeof(hdr));
	data += sizeof(hdr);
	remaining -= sizeof(hdr);

	if (remaining != hdr.len) {
		LOG_ERR("Event payload length is not correct");
		return NULL;
	}
	LOG_DBG("len %u", hdr.len);

	buf = bt_buf_get_evt(hdr.evt, discardable, K_NO_WAIT);
	if (!buf) {
		if (discardable) {
			LOG_DBG("Discardable buffer pool full, ignoring event");
		} else {
			LOG_ERR("No available event buffers!");
		}
		return buf;
	}

	net_buf_add_mem(buf, &hdr, sizeof(hdr));

	buf_tailroom = net_buf_tailroom(buf);
	if (buf_tailroom < remaining) {
		LOG_ERR("Not enough space in buffer %zu/%zu", remaining, buf_tailroom);
		net_buf_unref(buf);
		return NULL;
	}

	net_buf_add_mem(buf, data, remaining);

	return buf;
}

static struct net_buf *bt_esp_acl_recv(uint8_t *data, size_t remaining)
{
	struct bt_hci_acl_hdr hdr;
	struct net_buf *buf;
	size_t buf_tailroom;

	if (remaining < sizeof(hdr)) {
		LOG_ERR("Not enough data for ACL header");
		return NULL;
	}

	buf = bt_buf_get_rx(BT_BUF_ACL_IN, K_NO_WAIT);
	if (buf) {
		memcpy((void *)&hdr, data, sizeof(hdr));
		data += sizeof(hdr);
		remaining -= sizeof(hdr);

		net_buf_add_mem(buf, &hdr, sizeof(hdr));
	} else {
		LOG_ERR("No available ACL buffers!");
		return NULL;
	}

	if (remaining != sys_le16_to_cpu(hdr.len)) {
		LOG_ERR("ACL payload length is not correct");
		net_buf_unref(buf);
		return NULL;
	}

	buf_tailroom = net_buf_tailroom(buf);
	if (buf_tailroom < remaining) {
		LOG_ERR("Not enough space in buffer %zu/%zu", remaining, buf_tailroom);
		net_buf_unref(buf);
		return NULL;
	}

	LOG_DBG("len %u", remaining);
	net_buf_add_mem(buf, data, remaining);

	return buf;
}

static struct net_buf *bt_esp_iso_recv(uint8_t *data, size_t remaining)
{
	struct bt_hci_iso_hdr hdr;
	struct net_buf *buf;
	size_t buf_tailroom;

	if (remaining < sizeof(hdr)) {
		LOG_ERR("Not enough data for ISO header");
		return NULL;
	}

	buf = bt_buf_get_rx(BT_BUF_ISO_IN, K_NO_WAIT);
	if (buf) {
		memcpy((void *)&hdr, data, sizeof(hdr));
		data += sizeof(hdr);
		remaining -= sizeof(hdr);

		net_buf_add_mem(buf, &hdr, sizeof(hdr));
	} else {
		LOG_ERR("No available ISO buffers!");
		return NULL;
	}

	if (remaining != bt_iso_hdr_len(sys_le16_to_cpu(hdr.len))) {
		LOG_ERR("ISO payload length is not correct");
		net_buf_unref(buf);
		return NULL;
	}

	buf_tailroom = net_buf_tailroom(buf);
	if (buf_tailroom < remaining) {
		LOG_ERR("Not enough space in buffer %zu/%zu", remaining, buf_tailroom);
		net_buf_unref(buf);
		return NULL;
	}

	LOG_DBG("len %zu", remaining);
	net_buf_add_mem(buf, data, remaining);

	return buf;
}

static struct net_buf *hci_raw_to_net_buf(const struct hci_raw_item *item)
{
	uint8_t *data = item->data;
	size_t remaining = item->len;
	uint8_t pkt_indicator;
	struct net_buf *buf = NULL;

	if (remaining == 0U) {
		return NULL;
	}

	LOG_HEXDUMP_DBG(data, remaining, "host packet data:");

	pkt_indicator = *data++;
	remaining -= sizeof(pkt_indicator);

	switch (pkt_indicator) {
	case BT_HCI_H4_EVT:
		buf = bt_esp_evt_recv(data, remaining);
		break;
	case BT_HCI_H4_ACL:
		buf = bt_esp_acl_recv(data, remaining);
		break;
	case BT_HCI_H4_ISO:
		buf = bt_esp_iso_recv(data, remaining);
		break;
	default:
		LOG_ERR("Unknown HCI type %u", pkt_indicator);
		break;
	}

	return buf;
}

static bool hci_rx_process_one(const struct device *dev)
{
	struct bt_esp32_data *hci = dev->data;
	const uint32_t head = __atomic_load_n(&hci_ring_head_v, __ATOMIC_RELAXED);
	const uint32_t tail = __atomic_load_n(&hci_ring_tail_v, __ATOMIC_ACQUIRE);
	struct net_buf *buf;

	if (head == tail) {
		return false;
	}

	buf = hci_raw_to_net_buf(&hci_raw_ring[head]);
	__atomic_store_n(&hci_ring_head_v, (head + 1U) % HCI_RAW_Q_DEPTH, __ATOMIC_RELEASE);

	if (buf == NULL || hci->recv == NULL) {
		net_buf_unref(buf);
		return true;
	}

	LOG_DBG("Deferred bt_recv(%p)", buf);
	(void)hci->recv(dev, buf);

	return true;
}

static void IRAM_ATTR hci_rx_thread_fn(void *p1, void *p2, void *p3)
{
	const struct device *dev = p1;
	uint32_t last_drops = 0;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (atomic_get(&hci_rx_running) != 0) {
		const uint32_t drops = __atomic_load_n(&hci_ring_drops_v, __ATOMIC_RELAXED);

		if (drops != last_drops) {
			LOG_WRN("HCI raw ring full — dropped %u packets", drops - last_drops);
			last_drops = drops;
		}

		if (!hci_rx_process_one(dev)) {
			k_msleep(1);
		}
	}
}

static int IRAM_ATTR hci_esp_host_rcv_pkt(uint8_t *data, uint16_t len)
{
	uint32_t tail;
	uint32_t next;
	uint32_t head;

	/* btController context — memcpy + atomics only (no Zephyr calls). */
	if (len == 0U || len > HCI_RAW_MAX) {
		return -1;
	}

	tail = __atomic_load_n(&hci_ring_tail_v, __ATOMIC_RELAXED);
	next = (tail + 1U) % HCI_RAW_Q_DEPTH;
	head = __atomic_load_n(&hci_ring_head_v, __ATOMIC_ACQUIRE);
	if (next == head) {
		__atomic_fetch_add(&hci_ring_drops_v, 1U, __ATOMIC_RELAXED);
		return 0;
	}

	memcpy(hci_raw_ring[tail].data, data, len);
	hci_raw_ring[tail].len = len;
	__atomic_store_n(&hci_ring_tail_v, next, __ATOMIC_RELEASE);

	return 0;
}

static void IRAM_ATTR hci_esp_controller_rcv_pkt_ready(void)
{
	/* btController — intentionally empty; TX polls esp_vhci_host_check_send_available(). */
}

static esp_vhci_host_callback_t vhci_host_cb = {
	hci_esp_controller_rcv_pkt_ready,
	hci_esp_host_rcv_pkt
};

static int bt_esp32_send(const struct device *dev, struct net_buf *buf)
{
	int err = 0;
	uint8_t pkt_indicator;

	ARG_UNUSED(dev);

	LOG_DBG("buf %p type %u len %u", buf, bt_buf_get_type(buf), buf->len);

	switch (bt_buf_get_type(buf)) {
	case BT_BUF_ACL_OUT:
		pkt_indicator = BT_HCI_H4_ACL;
		break;
	case BT_BUF_CMD:
		pkt_indicator = BT_HCI_H4_CMD;
		break;
	case BT_BUF_ISO_OUT:
		pkt_indicator = BT_HCI_H4_ISO;
		break;
	default:
		LOG_ERR("Unknown type %u", bt_buf_get_type(buf));
		goto done;
	}
	net_buf_push_u8(buf, pkt_indicator);

	LOG_HEXDUMP_DBG(buf->data, buf->len, "Final HCI buffer:");

	{
		int waited = 0;

		while (!esp_vhci_host_check_send_available()) {
			k_msleep(HCI_SEND_POLL_MS);
			if (++waited >= HCI_SEND_TIMEOUT_MS) {
				LOG_ERR("Send packet timeout error");
				err = -ETIMEDOUT;
				goto done;
			}
		}
	}

	esp_vhci_host_send_packet(buf->data, buf->len);

done:
	net_buf_unref(buf);

	return err;
}

static int bt_esp32_ble_init(void)
{
	int ret;
	esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

#if defined(CONFIG_BT_CLASSIC) && defined(CONFIG_SOC_SERIES_ESP32)
	esp_bt_mode_t mode = ESP_BT_MODE_BTDM;
#else
	esp_bt_mode_t mode = ESP_BT_MODE_BLE;
#endif

	ret = esp_bt_controller_init(&bt_cfg);
	if (ret) {
		LOG_ERR("Bluetooth controller init failed %d", ret);
		return ret;
	}

	ret = esp_bt_controller_enable(mode);
	if (ret) {
		LOG_ERR("Bluetooth controller enable failed: %d", ret);
		return ret;
	}

	esp_vhci_host_register_callback(&vhci_host_cb);

	return 0;
}

static int bt_esp32_ble_deinit(void)
{
	int ret;

	ret = esp_bt_controller_disable();
	if (ret) {
		LOG_ERR("Bluetooth controller disable failed %d", ret);
		return ret;
	}

	ret = esp_bt_controller_deinit();
	if (ret) {
		LOG_ERR("Bluetooth controller deinit failed %d", ret);
		return ret;
	}

	return 0;
}

static int bt_esp32_open(const struct device *dev, bt_hci_recv_t recv)
{
	struct bt_esp32_data *hci = dev->data;
	int err;

	__atomic_store_n(&hci_ring_head_v, 0U, __ATOMIC_RELAXED);
	__atomic_store_n(&hci_ring_tail_v, 0U, __ATOMIC_RELAXED);
	__atomic_store_n(&hci_ring_drops_v, 0U, __ATOMIC_RELAXED);
	atomic_set(&hci_rx_running, 1);
	k_thread_create(&hci_rx_thread, hci_rx_stack, K_THREAD_STACK_SIZEOF(hci_rx_stack),
			hci_rx_thread_fn, (void *)dev, NULL, NULL,
			K_PRIO_COOP(CONFIG_BT_RX_PRIO > 0 ? CONFIG_BT_RX_PRIO - 1 : CONFIG_BT_RX_PRIO),
			0, K_NO_WAIT);
	k_thread_name_set(&hci_rx_thread, "ESP VHCI RX");

	err = bt_esp32_ble_init();
	if (err) {
		atomic_set(&hci_rx_running, 0);
		k_thread_join(&hci_rx_thread, K_MSEC(500));
		return err;
	}

	hci->recv = recv;

	LOG_INF("ESP32 BT started (SPSC VHCI ring depth=%u, BTC-safe)", HCI_RAW_Q_DEPTH);

	return 0;
}

static int bt_esp32_close(const struct device *dev)
{
	struct bt_esp32_data *hci = dev->data;
	int err;

	atomic_set(&hci_rx_running, 0);
	k_thread_join(&hci_rx_thread, K_MSEC(500));

	__atomic_store_n(&hci_ring_head_v, 0U, __ATOMIC_RELAXED);
	__atomic_store_n(&hci_ring_tail_v, 0U, __ATOMIC_RELAXED);

	err = bt_esp32_ble_deinit();
	if (err) {
		return err;
	}

	hci->recv = NULL;

	LOG_DBG("ESP32 BT stopped");

	return 0;
}

static const struct bt_hci_driver_api drv = {
	.open = bt_esp32_open,
	.send = bt_esp32_send,
	.close = bt_esp32_close,
};

#define BT_ESP32_DEVICE_INIT(inst)                                                                 \
	static struct bt_esp32_data bt_esp32_data_##inst = {                                       \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, NULL, NULL, &bt_esp32_data_##inst, NULL, POST_KERNEL,          \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &drv)

BT_ESP32_DEVICE_INIT(0)
