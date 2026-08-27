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
 * F0080003. Heart-rate start/stop = {0xD0,0x01}/{0xD0,0x00} (20-byte, zero
 * padded); SpO2 start/stop = {0x80,0x01,0x02}/{0x80,0x02,0x02}.
 *
 * The MT200 only keeps one LE link at a time, so this will fail to connect
 * whenever its companion phone app (H Band) already holds the BLE link.
 * Phone Bluetooth may stay on as long as it talks only to this ESP32.
 */

#define MT200_FLAG_HR    (1U << 0)
#define MT200_FLAG_SPO2  (1U << 1)
#define MT200_FLAG_STEPS (1U << 2)
#define MT200_FLAG_BAT   (1U << 3)

/** HCI 127 means N/A, not +127 dBm. Treat missing RSSI as the worst legal dBm. */
#define MT200_RSSI_UNAVAIL ((int8_t)-127)

/** Last decoded wearable sample. Integers only — safe to snprintf on the DATA path. */
struct mt200_telem {
	uint8_t hr;
	uint8_t spo2;
	uint32_t steps;
	uint8_t bat_pct;
	uint8_t flags;
	uint32_t seq;
	int8_t rssi;
};

/** Kick off scan -> connect -> subscribe -> HR + periodic steps/battery.
 *  No-op if a bridge session is already active. Restarts after drop. */
void mt200_bridge_start(void);

/** Schedule start a few seconds after BLE advertising is up (boot path). */
void mt200_bridge_autostart(void);

/** True while a scan/connect/session is in progress or connected. */
bool mt200_bridge_active(void);

/** Copy last samples (zeros / flags=0 if never seen or bridge compiled out). */
void mt200_bridge_telem(struct mt200_telem *out);
