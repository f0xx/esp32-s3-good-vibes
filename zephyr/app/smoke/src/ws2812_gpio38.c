/*
 * WS2812 on GPIO38 — bit-bang, same GRB frame as Arduino rgbLedWrite().
 * Cycle-counted WS2812B timings (see handshake copy for the stuck-channel note).
 */

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <esp_cpu.h>
#include <esp_rom_sys.h>
#include <soc/gpio_reg.h>

#include "ws2812_gpio38.h"

#define WS2812_PIN 6 /* GPIO38 = gpio1 pin 6 */

static const struct device *gpio1_dev;
static bool gpio_ready;

static inline void wait_until(uint32_t start, uint32_t ticks)
{
	while ((esp_cpu_get_cycle_count() - start) < ticks) {
	}
}

static void ws2812_send_bit(bool one)
{
	const uint32_t tpus = esp_rom_get_cpu_ticks_per_us();
	const uint32_t th = one ? ((tpus * 70U) / 100U) : ((tpus * 35U) / 100U);
	const uint32_t tl = one ? ((tpus * 60U) / 100U) : ((tpus * 80U) / 100U);
	const uint32_t t0 = esp_cpu_get_cycle_count();

	REG_WRITE(GPIO_OUT1_W1TS_REG, BIT(WS2812_PIN));
	wait_until(t0, th);
	REG_WRITE(GPIO_OUT1_W1TC_REG, BIT(WS2812_PIN));
	wait_until(t0, th + tl);
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

	REG_WRITE(GPIO_OUT1_W1TC_REG, BIT(WS2812_PIN));
	gpio_ready = true;
	return 0;
}

void ws2812_gpio38_rgb(uint8_t r, uint8_t g, uint8_t b)
{
	unsigned int key;

	if (!gpio_ready && ws2812_gpio38_init() != 0) {
		return;
	}

	REG_WRITE(GPIO_OUT1_W1TC_REG, BIT(WS2812_PIN));
	esp_rom_delay_us(300);

	key = irq_lock();
	ws2812_send_byte(r);
	ws2812_send_byte(g);
	ws2812_send_byte(b);
	irq_unlock(key);

	REG_WRITE(GPIO_OUT1_W1TC_REG, BIT(WS2812_PIN));
	esp_rom_delay_us(300);
}

void ws2812_gpio38_off(void)
{
	ws2812_gpio38_rgb(0, 0, 0);
}
