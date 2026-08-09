#include "device_profile.h"

#include <string.h>

enum power_profile device_profile_clamp(uint8_t v)
{
	if (v <= POWER_PROFILE_DC_FULL) {
		return (enum power_profile)v;
	}
	return POWER_PROFILE_BALANCED;
}

enum tft_policy device_profile_tft_policy(const struct device_config_v1 *cfg)
{
	if (cfg->tft_policy <= TFT_POLICY_ALWAYS) {
		return (enum tft_policy)cfg->tft_policy;
	}
	return TFT_POLICY_ALWAYS;
}

void device_profile_apply_preset(struct device_config_v1 *cfg, enum power_profile profile)
{
	if (cfg == NULL) {
		return;
	}

	cfg->power_profile = (uint8_t)profile;

	switch (profile) {
	case POWER_PROFILE_DEEP_SLEEP:
		cfg->tft_policy = TFT_POLICY_OFF;
		cfg->wake_interval_sec = 3600;
		cfg->active_window_sec = 90;
		cfg->cpu_mhz = 80;
		cfg->imu_sample_hz = 25;
		cfg->ble_poll_ms = 100;
		cfg->deep_sleep_enable = 1;
		break;
	case POWER_PROFILE_BALANCED:
		cfg->tft_policy = TFT_POLICY_ALWAYS;
		cfg->wake_interval_sec = 0;
		cfg->active_window_sec = 0;
		cfg->cpu_mhz = 80;
		cfg->imu_sample_hz = 10;
		cfg->ble_poll_ms = 200;
		cfg->deep_sleep_enable = 0;
		break;
	case POWER_PROFILE_PERFORMANCE:
		cfg->tft_policy = TFT_POLICY_ALWAYS;
		cfg->cpu_mhz = 160;
		cfg->imu_sample_hz = 50;
		cfg->ble_poll_ms = 100;
		cfg->deep_sleep_enable = 0;
		break;
	case POWER_PROFILE_DC_SAVE:
		cfg->tft_policy = TFT_POLICY_ALWAYS;
		cfg->cpu_mhz = 80;
		cfg->imu_sample_hz = 25;
		cfg->ble_poll_ms = 100;
		cfg->deep_sleep_enable = 0;
		break;
	case POWER_PROFILE_DC_FULL:
	default:
		cfg->tft_policy = TFT_POLICY_ALWAYS;
		cfg->cpu_mhz = 160;
		cfg->imu_sample_hz = 50;
		cfg->ble_poll_ms = 50;
		cfg->deep_sleep_enable = 0;
		break;
	}
}
