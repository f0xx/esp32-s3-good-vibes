/*
 * Flat-floor mounting calibration — a bubble-level-style *one-shot* correction, distinct from
 * the existing per-boot imu_cal_accel_level() bias zero.
 *
 * imu_cal_accel_level() (see imu_cal.h) re-zeros gyro/accel bias every boot on the *assumption*
 * the board happens to be flat at that instant — it can't know whether the sensor die is
 * actually mounted dead-level inside the enclosure, so any fixed mechanical mounting tilt gets
 * silently redefined as "zero" fresh each boot instead of being corrected.
 *
 * This module instead captures a persistent rotation matrix: point the device at a *known-true*
 * level reference (e.g. the bubble level in the reference photo) once, trigger a calibration
 * window, and it averages gravity over that window and solves for the rotation that maps the
 * measured (tilted) gravity vector onto canonical "flat" (0,0,1) — matching this codebase's
 * existing convention that az reads +1g at rest (see attitude.c's roll/pitch atan2 forms).
 * That matrix is stored in flash and applied to every subsequent sample (imu_pipeline_read_raw)
 * *in addition to* the existing per-boot bias zero, so it survives reboots and re-corrects the
 * already-calibrated stream consistently regardless of what orientation the board booted in.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "imu_math.h"
#include "imu_sample.h"

#define FLOOR_CALIB_DEFAULT_DURATION_MS 3000U
#define FLOOR_CALIB_MIN_DURATION_MS     500U
#define FLOOR_CALIB_MAX_DURATION_MS     15000U

void floor_calib_init(void);
/** Deferred flash flush — call every main-loop tick (see flash_safety.h: this must not fire
 *  while BLE is connected, which is the overwhelmingly common case for this feature). */
void floor_calib_poll(void);

/** Begin averaging gravity for duration_ms (clamped to [MIN,MAX]; 0 = default). Aborts any
 *  in-progress window and restarts. Device must be held still on the true-level reference for
 *  the whole window. */
void floor_calib_start(uint16_t duration_ms);
/** Discard the stored correction (back to identity) — deferred flash write like everything
 *  else that touches flash_area_erase(). */
void floor_calib_clear(void);

/** Feed one already-bias-calibrated, already-scaled sample (pre floor correction) into the
 *  in-progress averaging window, if any. No-op if not currently sampling. */
void floor_calib_feed(const struct imu_sample *sample);
/** Rotate sample's ax/ay/az and gx/gy/gz by the stored correction (identity/no-op if none
 *  stored yet). Call after floor_calib_feed() in the read path. */
void floor_calib_apply(struct imu_sample *sample);

bool floor_calib_valid(void);
bool floor_calib_sampling(void);
/** 0..1 progress through the current averaging window (0 if not sampling). */
float floor_calib_progress(void);
/** Angle (deg) between the last-measured raw gravity vector and true level — i.e. how much
 *  mounting tilt the last completed calibration corrected. 0 if never calibrated. */
float floor_calib_residual_deg(void);

/** Compact JSON status for BLE FLOORCAL read characteristic. Returns written length. */
int floor_calib_status_json(char *buf, size_t buf_len);
