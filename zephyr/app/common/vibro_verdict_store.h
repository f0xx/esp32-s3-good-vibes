#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vibro_capture.h"
#include "vibro_features.h"
#include "vibro_band_rms.h"

#define VERDICT_SPOOL_SLOTS 8U

int vibro_verdict_store_init(void);
uint32_t vibro_verdict_store_alloc_seq(void);
int vibro_verdict_store_append(uint32_t seq, uint32_t uptime_ms, const struct vibro_verdict *verdict,
			       const struct vibro_edge_features *edge,
			       const struct vibro_band_rms *bands);
bool vibro_verdict_store_ack(uint32_t seq);
uint16_t vibro_verdict_store_pending_count(void);
uint32_t vibro_verdict_store_last_seq(void);
uint32_t vibro_verdict_store_first_pending_seq(void);
/** Scratch spool partition usage (4096 B cap — internal flash, not SD). */
void vibro_verdict_store_spool_stats(uint32_t *cap_bytes, uint32_t *used_bytes,
				     uint16_t *pending_out);
