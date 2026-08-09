#pragma once

#include <stdint.h>

void ws2812_gpio38_rgb(uint8_t r, uint8_t g, uint8_t b);
void ws2812_gpio38_off(void);
int ws2812_gpio38_init(void);
