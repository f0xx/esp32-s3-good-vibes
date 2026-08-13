/*
 * Flash-backed reference-profile store (profiles.txt #2: "operator records
 * up to 5 ideal sampling profiles of the length of 30s max"). Each slot
 * holds a compact "fingerprint" of a reference recording — band-RMS vector,
 * scalar RMS/peak, and a magnitude-only time series (not the full 7-axis
 * IMU sample) for time-domain correlation — not the raw waveform, which
 * would be ~7x larger for no accuracy benefit since verdicts only ever
 * compare magnitude.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define VIBRO_REF_STORE_SLOTS 5U
#define VIBRO_REF_NAME_MAX    24U
#define VIBRO_REF_MAG_MAX     512U
#define VIBRO_REF_BAND_COUNT  4U

struct vibro_ref_profile {
	char name[VIBRO_REF_NAME_MAX];
	uint32_t created_unix; /* 0 if clock wasn't synced at record time */
	uint32_t updated_unix;
	uint32_t duration_ms;
	float sample_hz;
	uint32_t mag_len;
	float rms;
	float peak;
	float band_rms[VIBRO_REF_BAND_COUNT];
	bool band_valid;
	float mag[VIBRO_REF_MAG_MAX];
};

int vibro_ref_store_init(void);
bool vibro_ref_store_valid(uint8_t slot);
int vibro_ref_store_read(uint8_t slot, struct vibro_ref_profile *out);
int vibro_ref_store_write(uint8_t slot, const struct vibro_ref_profile *prof);
int vibro_ref_store_delete(uint8_t slot);
/** Pass -1 to clear (no active reference). */
int vibro_ref_store_set_active(int8_t slot);
int8_t vibro_ref_store_active_slot(void);
/** Compact metadata for all slots (no mag[]/band arrays) — for BLE listing. */
int vibro_ref_store_list_json(char *buf, size_t len);
