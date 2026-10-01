/*
 * Waveshare ESP32-S3-LCD-1.47B smoke test:
 * - ST7789 corners via shared renderer (sole display_write site)
 * - WS2812 off via GPIO38 bitbang
 * - BOOT press → yellow bar flash + serial log
 * - BLE connectable advertising (nRF Connect)
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "display_panel.h"
#include "renderer.h"
#include "ws2812_gpio38.h"

LOG_MODULE_REGISTER(smoke, LOG_LEVEL_INF);

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
	const uint16_t w = 48;
	const uint16_t h = 48;
	const uint16_t colors[4] = {PANEL_RED, PANEL_GREEN, PANEL_BLUE, PANEL_GREY};
	const uint16_t xs[4] = {0, PANEL_W - w, PANEL_W - w, 0};
	const uint16_t ys[4] = {0, 0, PANEL_H - h, PANEL_H - h};

	renderer_begin(PANEL_BLACK);
	for (int i = 0; i < 4; i++) {
		renderer_fill_rect(xs[i], ys[i], w, h, colors[i]);
	}
	renderer_present();
	(void)renderer_request_hw(true);
}

static void boot_feedback(void)
{
	const uint16_t y0 = PANEL_H / 2 - PANEL_BAR_H / 2;

	renderer_begin(PANEL_BLACK);
	renderer_fill_rect(0, y0, PANEL_W, PANEL_BAR_H, PANEL_YELLOW);
	renderer_present();
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

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("BLE advertising failed (%d)", err);
		return;
	}

	LOG_INF("BLE advertising as \"%s\"", CONFIG_BT_DEVICE_NAME);
}

static void input_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);
	if (evt->type == INPUT_EV_KEY && evt->code == INPUT_KEY_0 && evt->value != 0) {
		boot_presses++;
		boot_pending = true;
	}
}

INPUT_CALLBACK_DEFINE(NULL, input_cb, NULL);

int main(void)
{
	if (!device_is_ready(display_dev)) {
		LOG_ERR("display not ready");
		return 0;
	}

	LOG_INF("Waveshare 1.47B smoke — LCD + WS2812 + BOOT + BLE");
	ws2812_off();

	/* Smoke is single-threaded: claim main as the sole SPI owner. */
	renderer_init(display_dev);
	renderer_claim_thread();
	(void)renderer_request_hw(true);
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
