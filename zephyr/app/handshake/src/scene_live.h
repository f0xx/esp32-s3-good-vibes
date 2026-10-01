#pragma once

#include <stdbool.h>
#include <zephyr/device.h>

void scene_live_init(const struct device *display);
void scene_live_draw(const struct device *display);

/** Pause TFT SPI flush while flash_area_erase runs (avoids TG0WDT with BT radio). */
void scene_live_set_flash_quiet(bool quiet);
bool scene_live_flash_quiet(void);

