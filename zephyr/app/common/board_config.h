/*
 * Waveshare ESP32-S3-LCD-1.47B — shared constants (parity with esp32 board_config.h)
 */

#pragma once

#define PANEL_TFT_BL_DEFAULT_PERCENT  40
#define PANEL_TFT_BL_MAX_PERCENT      50

#define NET_BOOT_ERASE_MS             10000U
#define NVS_BOOT_ERASE_MS             15000U
#define NET_PROFILE_MAX               8U
#define NET_STORE_NAMESPACE           "netcfg"

#define BAT_ADC_CHANNEL               0 /* GPIO1 = ADC1 CH0 */
#define BAT_ADC_R_HIGH_OHM            200000.0f
#define BAT_ADC_R_LOW_OHM             100000.0f
#define BAT_MEASUREMENT_OFFSET        0.992857f
#define BAT_FULL_V                    4.20f
#define BAT_EMPTY_V                   3.00f
#define BAT_DC_MARGIN_V               4.18f
#define BAT_TREND_WINDOW              16
#define BAT_TREND_COMPARE             5
#define BAT_TREND_RISE_V              0.012f
#define BAT_TREND_FALL_V              -0.010f
#define BAT_TREND_STABLE_V            0.004f
#define BAT_ADC_SAMPLES               4
#define BAT_SAMPLE_MS                 500
#define BAT_VOLTAGE_EMA               0.25f
#define BAT_EXTERNAL_V                3.25f
#define BAT_ADC_MIN_V                 0.40f

#define IMU_SAMPLE_HZ_DEFAULT         100
#define IMU_SAMPLE_HZ_MIN             10
#define RENDER_HZ_DEFAULT             30

/*
 * Demo/staging operating mode — cycled by BOOT tap (device_config local flag).
 * CPU steps are the only three PLL frequencies the ESP32-S3 clock driver
 * accepts (80/160/240 MHz); 120/40 MHz requested in the spec aren't valid PLL
 * dividers, so each tier below picks the closest supported step.
 */
#define OPMODE_CPU_MHZ_DEMO_DC        240U
#define OPMODE_CPU_MHZ_DEMO_BAT       160U
#define OPMODE_CPU_MHZ_STAGING_DC     160U
#define OPMODE_CPU_MHZ_STAGING_BAT    80U

#define VIBRO_WARN_RMS_DELTA_G        0.05f
#define VIBRO_ALERT_RMS_DELTA_G       0.15f
#define VIBRO_WARN_CORR               0.85f
#define VIBRO_ALERT_CORR              0.70f
#define VIBRO_WARN_BAND_CORR          0.80f
#define VIBRO_ALERT_BAND_CORR         0.60f
#define VIBRO_WARN_BAND_DELTA           0.15f
#define VIBRO_ALERT_BAND_DELTA          0.35f

#define CHIP_TEMP_SAMPLE_MS           2000U

enum power_profile {
	POWER_PROFILE_DEEP_SLEEP = 0,
	POWER_PROFILE_BALANCED = 1,
	POWER_PROFILE_PERFORMANCE = 2,
	POWER_PROFILE_DC_SAVE = 3,
	POWER_PROFILE_DC_FULL = 5,
};

enum tft_policy {
	TFT_POLICY_OFF = 0,
	TFT_POLICY_ON_DEMAND = 1,
	TFT_POLICY_ALWAYS = 2,
};
