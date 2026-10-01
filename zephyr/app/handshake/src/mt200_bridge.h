#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * Experimental BLE-central bridge to a Veepoo/H-Band "MT200" smart clock.
 *
 * Protocol recovered by decompiling Veepoo's own published SDK binaries
 * (github.com/HBandSDK/Android_Ble_SDK, vpprotocol-2.3.80.15.aar) and verified
 * against a live GATT scan of the actual device: primary service
 * F0080001-0451-4000-B000-000000000000, notify char F0080002, write char
 * F0080003. After CCC subscribe the bridge must send confirmDevicePwd
 * (opcode A1 + PIN 0000 + wall clock) before A8/D8/A0/HR — otherwise the
 * watch only emits A1 ID frames and wok stays 0. settingTime (opcode A5)
 * pushes wall clock only after clock_sync has phone/NTP time (not raw uptime),
 * then every 30 min while linked; also on phone TIME corrections.
 * Heart-rate start/stop = {0xD0,0x01}/{0xD0,0x00} (20-byte, zero padded);
 * SpO2 start/stop = {0x80,0x01,0x02}/{0x80,0x02,0x02}. G-sensor sport =
 * {0xF1,0x20}; sport-model steps/kcal/distance = {0xD8,0x00}. SpO2 shares
 * the PPG with HR, so the bridge time-slices a short SpO2 window.
 *
 * The MT200 only keeps one LE link at a time, so this will fail to connect
 * whenever its companion phone app (H Band) already holds the BLE link.
 * Phone Bluetooth may stay on as long as it talks only to this ESP32.
 */

#define MT200_FLAG_HR    (1U << 0)
#define MT200_FLAG_SPO2  (1U << 1)
#define MT200_FLAG_STEPS (1U << 2)
#define MT200_FLAG_BAT   (1U << 3)
#define MT200_FLAG_KCAL  (1U << 4)
#define MT200_FLAG_DIST  (1U << 5)

/** HCI 127 means N/A, not +127 dBm. Treat missing RSSI as the worst legal dBm. */
#define MT200_RSSI_UNAVAIL ((int8_t)-127)

/** Last decoded wearable sample. Integers only — safe to snprintf on the DATA path. */
struct mt200_telem {
	uint8_t hr;
	uint8_t spo2;
	uint32_t steps;
	uint8_t bat_pct;
	uint32_t kcal_x10; /* D8 kcal * 10 (36 = 3.6 kcal) */
	uint32_t dist_mm;  /* D8 distance, millimetres */
	uint8_t flags;
	uint32_t seq;
	int8_t rssi;
};

/** Kick off scan -> connect -> subscribe -> HR + periodic steps/battery.
 *  No-op if a bridge session is already active. Restarts after drop. */
void mt200_bridge_start(void);

/** Schedule start a few seconds after BLE advertising is up (boot path). */
void mt200_bridge_autostart(void);

/** Drop the watch link (and inhibit reconnect) so WiFi can use the 2.4 GHz radio.
 *  Safe from net_mgmt / GATT / ISR: only sets flags + queues BT work. */
void mt200_bridge_pause(void);
/** Allow the watch bridge to autostart again after WiFi scan/connect.
 *  Safe from net_mgmt / GATT / ISR (queued). */
void mt200_bridge_resume(void);

/**
 * Phone peripheral link holds the radio: drop MT200 central and refuse
 * start/resume until cleared. Prevents dual-role HCI starvation that freezes
 * the cube and leaves phone BLE stuck. Safe from BT callbacks (queued).
 */
void mt200_bridge_set_phone_hold(bool hold);

/** True while MT200 feature is enabled (including quiet gaps between sessions). */
bool mt200_bridge_wanted(void);

/** True while a granulated session owns the radio (scan/connect/fetch). */
bool mt200_bridge_radio_busy(void);

/** Quiet-window panel refresh: true while a small SPI frame budget remains. */
bool mt200_bridge_quiet_spi_ok(void);
void mt200_bridge_quiet_spi_consumed(void);

/**
 * True while scan/connect/GATT is live (alias of radio_busy).
 * Quiet gaps return false so panel SPI / phone adv can run between sessions.
 */
bool mt200_bridge_active(void);

/** Copy last samples (zeros / flags=0 if never seen or bridge compiled out). */
void mt200_bridge_telem(struct mt200_telem *out);

/** Push Veepoo settingTime (A5) from clock_sync wall time if the watch is linked
 *  and ESP already has phone/NTP wall clock (no-op if still on uptime). */
void mt200_bridge_sync_time(void);
