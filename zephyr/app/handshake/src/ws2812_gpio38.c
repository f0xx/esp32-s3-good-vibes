/*
 * WS2812 on GPIO38 — ESP32-S3 RMT TX, same encoding as Arduino rgbLedWrite().
 *
 * Arduino RMT timings (10 MHz; 0-bit 4+8 ticks, 1-bit 8+4 ticks).
 * This pixel is RGB on the wire (v183 GRB had R↔G swapped on the bench).
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <esp_rom_gpio.h>
#include <hal/rmt_ll.h>
#include <soc/gpio_sig_map.h>
#include <soc/rmt_struct.h>
#include <soc/soc_caps.h>

#include "ws2812_gpio38.h"

LOG_MODULE_REGISTER(ws2812, LOG_LEVEL_INF);

#define WS2812_GPIO      38
#define WS2812_GPIO1_PIN 6 /* GPIO38 = gpio1 pin 6 */
#define RMT_CH           0U
/* Arduino rgbLedWrite: 10 MHz RMT tick (100 ns). */
#define RMT_TICK_0H      4U
#define RMT_TICK_0L      8U
#define RMT_TICK_1H      8U
#define RMT_TICK_1L      4U
/* ESP32-S3: 4 TX + 4 RX channels in one group (SOC_RMT_CHANNELS_PER_GROUP gone in 4.x). */
#ifndef SOC_RMT_CHANNELS_PER_GROUP
#define SOC_RMT_CHANNELS_PER_GROUP 8
#endif

typedef struct {
	struct {
		volatile uint32_t data32[SOC_RMT_MEM_WORDS_PER_CHANNEL];
	} chan[SOC_RMT_CHANNELS_PER_GROUP];
} rmt_mem_map_t;

extern rmt_mem_map_t RMTMEM;

static const struct device *gpio1_dev;
static bool rmt_ready;

static uint32_t rmt_symbol(bool one)
{
	const uint32_t th = one ? RMT_TICK_1H : RMT_TICK_0H;
	const uint32_t tl = one ? RMT_TICK_1L : RMT_TICK_0L;

	return (th) | (1U << 15) | (tl << 16);
}

static void rmt_encode_byte(uint32_t *dst, uint8_t byte)
{
	for (int i = 7; i >= 0; i--) {
		*dst++ = rmt_symbol((byte >> i) & 1);
	}
}

static int ws2812_rmt_hw_init(void)
{
	gpio1_dev = DEVICE_DT_GET(DT_NODELABEL(gpio1));
	if (!device_is_ready(gpio1_dev)) {
		return -ENODEV;
	}
	if (gpio_pin_configure(gpio1_dev, WS2812_GPIO1_PIN, GPIO_OUTPUT_INACTIVE) != 0) {
		return -EIO;
	}

	/* Zephyr 4.x / new Espressif HAL: bus clock + group clock (no PERIPH_RMT_MODULE).
	 * __DECLARE_RCC_ATOMIC_ENV is force-defined empty on the compile line. */
	rmt_ll_enable_bus_clock(0, true);
	rmt_ll_reset_register(0);
	rmt_ll_enable_group_clock(&RMT, true);
	rmt_ll_mem_power_by_pmu(&RMT);
	rmt_ll_enable_mem_access_nonfifo(&RMT, true);
	rmt_ll_set_group_clock_src(&RMT, RMT_CH, RMT_CLK_SRC_APB, 1, 0, 0);
	rmt_ll_tx_set_channel_clock_div(&RMT, RMT_CH, 8);
	rmt_ll_tx_set_mem_blocks(&RMT, RMT_CH, 1);
	rmt_ll_tx_enable_wrap(&RMT, RMT_CH, false);
	rmt_ll_tx_enable_loop(&RMT, RMT_CH, false);
	rmt_ll_tx_enable_carrier_modulation(&RMT, RMT_CH, false);
	rmt_ll_tx_fix_idle_level(&RMT, RMT_CH, 0, true);
	rmt_ll_tx_reset_pointer(&RMT, RMT_CH);

	esp_rom_gpio_connect_out_signal(WS2812_GPIO, RMT_SIG_OUT0_IDX, false, false);

	rmt_ready = true;
	LOG_INF("WS2812 RMT TX ch%u @ 10 MHz on GPIO%u (Arduino rgbLedWrite timings)", RMT_CH,
		WS2812_GPIO);
	return 0;
}

int ws2812_gpio38_init(void)
{
	if (rmt_ready) {
		return 0;
	}
	return ws2812_rmt_hw_init();
}

void ws2812_gpio38_rgb(uint8_t r, uint8_t g, uint8_t b)
{
	uint32_t items[25];
	uint32_t deadline;

	if (!rmt_ready && ws2812_gpio38_init() != 0) {
		return;
	}

	/* Bench map (v183): programmed R→visual G, G→R, B→B. Wire is RGB. */
	rmt_encode_byte(&items[0], r);
	rmt_encode_byte(&items[8], g);
	rmt_encode_byte(&items[16], b);
	items[24] = 0;

	rmt_ll_tx_stop(&RMT, RMT_CH);
	rmt_ll_tx_reset_pointer(&RMT, RMT_CH);
	rmt_ll_clear_interrupt_status(&RMT, RMT_LL_EVENT_TX_DONE(RMT_CH));

	for (uint32_t i = 0; i < 25U; i++) {
		RMTMEM.chan[RMT_CH].data32[i] = items[i];
	}

	rmt_ll_tx_start(&RMT, RMT_CH);

	deadline = k_cycle_get_32() + k_ms_to_cyc_ceil32(2);
	while ((RMT.int_raw.val & RMT_LL_EVENT_TX_DONE(RMT_CH)) == 0U) {
		if ((int32_t)(k_cycle_get_32() - deadline) >= 0) {
			LOG_WRN("WS2812 RMT TX timeout");
			rmt_ll_tx_stop(&RMT, RMT_CH);
			break;
		}
	}
	rmt_ll_clear_interrupt_status(&RMT, RMT_LL_EVENT_TX_DONE(RMT_CH));
}

void ws2812_gpio38_grb(uint8_t green, uint8_t red, uint8_t blue)
{
	ws2812_gpio38_rgb(red, green, blue);
}

void ws2812_gpio38_off(void)
{
	ws2812_gpio38_rgb(0, 0, 0);
}
