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
/* There is no VBUS/charger-status sense pin on this board — DC vs battery is inferred purely
 * from cell voltage + trend (see battery_monitor.c classify_power_source()). A LiPo that
 * finished charging on USB commonly plateaus/trickles at ~4.15-4.19V depending on charger
 * termination voltage and this board's ADC calibration slop (BAT_MEASUREMENT_OFFSET), which
 * sat just *below* the old 4.18V margin — so a fully-charged-on-USB device could get stuck
 * reporting src=BAT forever (flat trend never triggers the rise-based DC detection, and the
 * margin check that would otherwise catch it never fired). Lowered to comfortably cover that
 * plateau while staying well above any real discharge-under-load resting voltage; an actual
 * unplug is still caught quickly via the trend-fall path (BAT_TREND_FALL_V), independent of
 * this margin. */
#define BAT_DC_MARGIN_V               4.10f
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
/** Screen-off / no-render awake tier — enough for BLE telemetry, not scene fidelity. */
#define IMU_SAMPLE_HZ_IDLE            25
#define RENDER_HZ_DEFAULT             30

/*
 * Demo/staging operating mode — cycled by BOOT tap (device_config local flag).
 * CPU steps are 80/160/240 MHz — all three are just divider taps off the same
 * always-on 480 MHz BBPLL, which is what makes live switching between them
 * safe while BT/BLE is active (see apply_cpu_mhz_fast() in power_manager.c).
 * 120 MHz isn't a valid PLL divider. Frequencies below 80 MHz (e.g. 40 MHz)
 * are technically supported by the SoC via the XTAL clock source instead of
 * the PLL, but that path disables the BBPLL outright and leaves the CPU too
 * slow to reliably service BLE's real-time interrupts — not offered while a
 * BT/BLE link may be active, so not exposed here.
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
