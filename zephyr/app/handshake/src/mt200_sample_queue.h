#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mt200_bridge.h"

/**
 * Offline / catch-up ring for MT200 wearable samples.
 *
 * Live path: enqueue on content change, rate-limited to 1 Hz (Veepoo live
 * sport/HR is ~1 Hz; A8/D8 polls are slower). Dedupe by content hash so
 * identical 10 s re-polls do not flood the ring.
 *
 * History path (not yet drained from watch): Veepoo origin packages are unique
 * by (calendar day, packageNumber 1..288) at 5-minute native rate — see
 * docs/veepoo-proto-ble-reverse.md (opcodes D1 classic / DF proto v3+v5).
 */

#define MT200_SAMPLE_QUEUE_CAP 2048U

struct mt200_sample {
	uint32_t esp_seq;
	uint32_t wall_ms; /* clock_sync wall ms when known, else uptime */
	uint32_t content_hash;
	uint8_t hr;
	uint8_t spo2;
	uint8_t bat_pct;
	uint8_t flags;
	uint32_t steps;
	uint32_t kcal_x10;
	uint32_t dist_mm;
	int8_t rssi;
	uint8_t _pad[3];
};

void mt200_sample_queue_init(void);

/** Enqueue if content changed and ≥1 s since last accept. Returns true if stored. */
bool mt200_sample_queue_offer_live(const struct mt200_telem *t, uint32_t wall_ms);

uint16_t mt200_sample_queue_pending(void);
uint16_t mt200_sample_queue_capacity(void);

/** Pop oldest; false if empty. */
bool mt200_sample_queue_pop(struct mt200_sample *out);

/** Peek oldest without removing. */
bool mt200_sample_queue_peek(struct mt200_sample *out);

/** Last accepted live esp_seq (0 if none). */
uint32_t mt200_sample_queue_last_seq(void);
