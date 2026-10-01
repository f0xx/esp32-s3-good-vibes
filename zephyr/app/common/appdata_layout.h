/*
 * Offsets below are relative to appdata_partition (256 KiB).
 * Every flash store must include this header — do not invent parallel offsets.
 *
 * Absolute map (procpu DTS, 3 MiB slots):
 *   mcuboot 0x0, image-0 0x10000, image-1 0x310000, scratch 0x610000,
 *   storage(NVS) 0x650000, coredump 0x658000, appdata 0x65c000.
 */
#pragma once

#include <zephyr/sys/util.h>

#define APPDATA_SECTOR_BYTES 4096U

#define APPDATA_CRASH_RING_A_OFF     0U
#define APPDATA_VIBRO_VERDICT_OFF    4096U
#define APPDATA_VIBRO_REF_HDR_OFF    8192U
#define APPDATA_VIBRO_REF_SLOTS_OFF  12288U
#define APPDATA_VIBRO_REF_SLOTS      5U
#define APPDATA_VIBRO_REF_END \
	(APPDATA_VIBRO_REF_SLOTS_OFF + (APPDATA_VIBRO_REF_SLOTS * APPDATA_SECTOR_BYTES))
#define APPDATA_CRASH_RING_B_OFF     32768U /* == APPDATA_VIBRO_REF_END */

/* Compile-time guard: ring B must sit immediately after vibro ref slots. */
BUILD_ASSERT(APPDATA_CRASH_RING_B_OFF == APPDATA_VIBRO_REF_END,
	     "crash_ring sector B must follow vibro_ref slots without overlap");
BUILD_ASSERT(APPDATA_VIBRO_VERDICT_OFF == APPDATA_CRASH_RING_A_OFF + APPDATA_SECTOR_BYTES,
	     "vibro verdict must follow crash_ring A");
BUILD_ASSERT(APPDATA_VIBRO_REF_HDR_OFF == APPDATA_VIBRO_VERDICT_OFF + APPDATA_SECTOR_BYTES,
	     "vibro ref hdr must follow verdict spool");
