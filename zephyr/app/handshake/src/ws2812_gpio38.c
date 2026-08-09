/*
 * WS2812 on GPIO38 — bit-bang (matches Arduino rgbLedWrite / RMT timing).
 * GRB wire order. Pin 38 lives in GPIO OUT1 bank (bit 6).
 *
 * Note: WS2812 is a digital NRZ protocol (~800 kHz), not analog PWM/DAC on
 * the data pin. Brightness is controlled via 8-bit G/R/B channel values.
 */

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <esp_rom_sys.h>
#include <soc/gpio_reg.h>

#include "ws2812_gpio38.h"

#define WS2812_PIN 6 /* GPIO38 = gpio1 pin 6 */

static const struct device *gpio1_dev;
static bool gpio_ready;

static inline void delay_short(void)
{
	__asm__ volatile("nop; nop; nop; nop; nop; nop; nop; nop");
}

static void ws2812_send_bit(bool one)
{
	if (one) {
		REG_WRITE(GPIO_OUT1_W1TS_REG, BIT(WS2812_PIN));
		esp_rom_delay_us(1);
		REG_WRITE(GPIO_OUT1_W1TC_REG, BIT(WS2812_PIN));
		delay_short();
	} else {
		REG_WRITE(GPIO_OUT1_W1TS_REG, BIT(WS2812_PIN));
		delay_short();
		REG_WRITE(GPIO_OUT1_W1TC_REG, BIT(WS2812_PIN));
		esp_rom_delay_us(1);
	}
}

static void ws2812_send_byte(uint8_t byte)
{
	for (int i = 7; i >= 0; i--) {
		ws2812_send_bit((byte >> i) & 1);
	}
}

int ws2812_gpio38_init(void)
{
	gpio1_dev = DEVICE_DT_GET(DT_NODELABEL(gpio1));

	if (!device_is_ready(gpio1_dev)) {
		return -ENODEV;
	}

	if (gpio_pin_configure(gpio1_dev, WS2812_PIN, GPIO_OUTPUT_INACTIVE) != 0) {
		return -EIO;
	}

	gpio_ready = true;
	return 0;
}

void ws2812_gpio38_rgb(uint8_t r, uint8_t g, uint8_t b)
{
	unsigned int key;

	if (!gpio_ready && ws2812_gpio38_init() != 0) {
		return;
	}

	key = irq_lock();
	ws2812_send_byte(g);
	ws2812_send_byte(r);
	ws2812_send_byte(b);
	irq_unlock(key);

	esp_rom_delay_us(300);
}

void ws2812_gpio38_off(void)
{
	ws2812_gpio38_rgb(0, 0, 0);
}
