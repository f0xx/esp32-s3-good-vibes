#pragma once

#include "board_config.h"
#include "device_config.h"

enum power_profile device_profile_clamp(uint8_t v);
enum tft_policy device_profile_tft_policy(const struct device_config_v1 *cfg);
void device_profile_apply_preset(struct device_config_v1 *cfg, enum power_profile profile);
