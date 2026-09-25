#pragma once

#include <stdint.h>

/** Logical GRB — converted to RGB on the wire. */
void ws2812_gpio38_grb(uint8_t green, uint8_t red, uint8_t blue);
/** Logical RGB — RGB on the wire via ESP32-S3 RMT. */
void ws2812_gpio38_rgb(uint8_t r, uint8_t g, uint8_t b);
void ws2812_gpio38_off(void);
int ws2812_gpio38_init(void);
