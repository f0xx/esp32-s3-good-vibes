/*
 * Waveshare ESP32-S3-LCD-1.47B smoke test:
 * - ST7789 corners (BGR panel)
 * - WS2812 off via GPIO38 bitbang
 * - BOOT press → yellow bar flash + serial log
 * - BLE connectable advertising (nRF Connect)
 */

#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "ws2812_gpio38.h"

LOG_MODULE_REGISTER(smoke, LOG_LEVEL_INF);

/* Panel shows G/B swapped vs naive RGB565 — match Waveshare/TFT_eSPI BGR. */
#define C_RED   0xF800
#define C_GREEN 0x001F
#define C_BLUE  0x07E0
#define C_GREY  0x8410
#define C_YELL  0xFFE0

static const struct device *const display_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

static struct gpio_dt_spec boot_btn = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static volatile uint32_t boot_presses;
static volatile bool boot_pending;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void ws2812_off(void)
{
	for (int i = 0; i < 3; i++) {
		ws2812_gpio38_off();
		k_msleep(5);
	}
	LOG_INF("WS2812 off (GPIO38 bitbang x3)");
}

static void draw_corners(void)
{
	struct display_capabilities caps;
	struct display_buffer_descriptor desc;
	static uint16_t buf[48 * 48];
	uint16_t colors[4] = {C_RED, C_GREEN, C_BLUE, C_GREY};
	int coords[4][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};

	display_get_capabilities(display_dev, &caps);

	const uint16_t w = 48;
	const uint16_t h = 48;

	coords[1][0] = caps.x_resolution - w;
	coords[2][0] = caps.x_resolution - w;
	coords[2][1] = caps.y_resolution - h;
	coords[3][1] = caps.y_resolution - h;

	desc.width = w;
	desc.height = h;
	desc.pitch = w;
	desc.buf_size = sizeof(buf);

	for (int i = 0; i < 4; i++) {
		for (size_t n = 0; n < ARRAY_SIZE(buf); n++) {
			buf[n] = colors[i];
		}
		display_write(display_dev, coords[i][0], coords[i][1], &desc, buf);
	}

	display_blanking_off(display_dev);
}

static void boot_feedback(void)
{
	struct display_capabilities caps;
	struct display_buffer_descriptor desc;
	static uint16_t buf[172 * 24];

	display_get_capabilities(display_dev, &caps);

	desc.width = caps.x_resolution;
	desc.height = 24;
	desc.pitch = caps.x_resolution;
	desc.buf_size = sizeof(buf);

	for (size_t n = 0; n < ARRAY_SIZE(buf); n++) {
		buf[n] = C_YELL;
	}

	display_write(display_dev, 0, caps.y_resolution / 2 - 12, &desc, buf);
	k_msleep(250);
	draw_corners();
}

static void start_ble(void)
{
	int err = bt_enable(NULL);

	if (err) {
		LOG_ERR("Bluetooth init failed (%d)", err);
		return;
	}

	err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("BLE advertising failed (%d)", err);
		return;
	}

	LOG_INF("BLE advertising as \"%s\"", CONFIG_BT_DEVICE_NAME);
}

static void input_cb(struct input_event *evt)
{
	if (evt->type == INPUT_EV_KEY && evt->code == INPUT_KEY_0 && evt->value != 0) {
		boot_presses++;
		boot_pending = true;
	}
}

INPUT_CALLBACK_DEFINE(NULL, input_cb);

int main(void)
{
	if (!device_is_ready(display_dev)) {
		LOG_ERR("display not ready");
		return 0;
	}

	LOG_INF("Waveshare 1.47B smoke — LCD + WS2812 + BOOT + BLE");
	ws2812_off();
	draw_corners();
	start_ble();
	LOG_INF("BOOT GPIO%d — press for yellow bar flash", boot_btn.pin);

	while (1) {
		if (boot_pending) {
			boot_pending = false;
			LOG_INF("BOOT pressed (%u)", boot_presses);
			boot_feedback();
		}

		k_msleep(20);
	}

	return 0;
}
