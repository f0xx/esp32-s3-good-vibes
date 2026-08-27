#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vibro_capture.h"

/*
 * Acrylic WS2812 indication (GPIO38) — hardcoded schema; edit firmware to change.
 *
 *  Blue solid          — no reference profiles; run mobile ref wizard first.
 *  Blue flash 2s/2s    — refs recorded, awaiting arm/start (wizard Finish or CMD 10).
 *  Red solid           — generic NOK (verdict warn/alert, missing reference, …).
 *  Yellow flash 2s/2s  — battery critical (≤10% SOC, on battery only).
 *  Green solid ~2s     — generic OK pulse (e.g. NVS save), then base state resumes.
 *  Off                 — operational (armed, no active fault).
 */

void vibro_led_init(void);

/** Main-looper tick — drives flash phases, battery/setup base state, OK pulse. */
void vibro_led_poll(void);

/** Verdict path — WARN/ALERT → red; OK clears active NOK. */
void vibro_led_on_verdict(enum vibro_level level);

/** Brief green OK indication (~2 s). */
void vibro_led_pulse_ok(void);
