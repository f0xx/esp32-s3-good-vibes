#pragma once

#include <zephyr/device.h>

void scene_live_init(const struct device *display);
void scene_live_draw(const struct device *display);
