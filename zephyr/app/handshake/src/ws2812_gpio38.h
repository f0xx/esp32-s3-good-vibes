#pragma once

#include <stdint.h>

/** Set GRB channel levels (WS2812 wire order on GPIO38 — not RGB parameter order). */
void ws2812_gpio38_grb(uint8_t green, uint8_t red, uint8_t blue);
/** Legacy name: r/g/b are logical red/green/blue, converted to GRB wire order internally. */
void ws2812_gpio38_rgb(uint8_t r, uint8_t g, uint8_t b);
void ws2812_gpio38_off(void);
int ws2812_gpio38_init(void);
