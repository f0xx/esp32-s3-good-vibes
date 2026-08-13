/*
 * Reference-profile flash store — same scratch_partition as crash_ring_store
 * (offset 0, 4KB) and vibro_verdict_store (offset 4096, 4KB). This store
 * starts at offset 8192: one 4KB header sector (active-slot pointer) plus
 * VIBRO_REF_STORE_SLOTS dedicated 4KB sectors (one per slot), so recording a
 * new profile into one slot only erases that slot's sector — the other
 * slots' recordings are untouched (unlike crash_ring's single shared sector,
 * which is fine for a wrap-around ring but would be destructive here).
 */

#include "vibro_ref_store.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(vibro_ref, LOG_LEVEL_INF);

#define VIBRO_REF_PARTITION    scratch_partition
#define VIBRO_REF_PARTITION_ID FIXED_PARTITION_ID(VIBRO_REF_PARTITION)

#if !FIXED_PARTITION_EXISTS(VIBRO_REF_PARTITION)
#error "vibro ref store requires scratch partition"
#endif

#define VIBRO_REF_HDR_OFFSET  8192U
#define VIBRO_REF_HDR_BYTES   4096U
#define VIBRO_REF_SLOTS_OFFSET (VIBRO_REF_HDR_OFFSET + VIBRO_REF_HDR_BYTES) /* 12288 */
#define VIBRO_REF_SLOT_BYTES  4096U

#define VIBRO_REF_HDR_MAGIC 0x52465248U /* RFRH */
#define VIBRO_REF_REC_MAGIC 0x52465252U /* RFRR */
#define VIBRO_REF_VERSION   1U

struct vibro_ref_header {
	uint32_t magic;
	int8_t active_slot; /* -1 = none */
	uint8_t _pad[3];
	uint32_t crc32;
} __packed;

struct vibro_ref_record {
	uint32_t magic;
	uint32_t version;
	uint8_t slot;
	uint8_t band_valid;
	uint8_t _pad0[2];
	char name[VIBRO_REF_NAME_MAX];
	uint32_t created_unix;
	uint32_t updated_unix;
	uint32_t duration_ms;
	float sample_hz;
	uint32_t mag_len;
	float rms;
	float peak;
	float band_rms[VIBRO_REF_BAND_COUNT];
	float mag[VIBRO_REF_MAG_MAX];
	uint32_t crc32;
} __packed;

BUILD_ASSERT(sizeof(struct vibro_ref_record) <= VIBRO_REF_SLOT_BYTES,
	     "vibro_ref_record must fit in one flash sector");

static struct vibro_ref_header g_hdr;
static bool g_loaded;

static uint32_t hdr_crc(const struct vibro_ref_header *hdr)
{
	return crc32_ieee((const uint8_t *)hdr, offsetof(struct vibro_ref_header, crc32));
}

static uint32_t rec_crc(const struct vibro_ref_record *rec)
{
	return crc32_ieee((const uint8_t *)rec, offsetof(struct vibro_ref_record, crc32));
}

static off_t slot_offset(uint8_t slot)
{
	return (off_t)(VIBRO_REF_SLOTS_OFFSET + (size_t)slot * VIBRO_REF_SLOT_BYTES);
}

static int write_header(void)
{
	const struct flash_area *fa;
	int err;

	g_hdr.crc32 = hdr_crc(&g_hdr);
	err = flash_area_open(VIBRO_REF_PARTITION_ID, &fa);
	if (err != 0) {
		return err;
	}

	err = flash_area_erase(fa, VIBRO_REF_HDR_OFFSET, VIBRO_REF_HDR_BYTES);
	if (err == 0) {
		err = flash_area_write(fa, VIBRO_REF_HDR_OFFSET, &g_hdr, sizeof(g_hdr));
	}
	flash_area_close(fa);
	return err;
}

static int read_header(void)
{
	const struct flash_area *fa;
	int err;

	err = flash_area_open(VIBRO_REF_PARTITION_ID, &fa);
	if (err != 0) {
		return err;
	}

	err = flash_area_read(fa, VIBRO_REF_HDR_OFFSET, &g_hdr, sizeof(g_hdr));
	flash_area_close(fa);
	if (err != 0) {
		return err;
	}

	if (g_hdr.magic != VIBRO_REF_HDR_MAGIC || g_hdr.crc32 != hdr_crc(&g_hdr)) {
		memset(&g_hdr, 0, sizeof(g_hdr));
		g_hdr.magic = VIBRO_REF_HDR_MAGIC;
		g_hdr.active_slot = -1;
		return write_header();
	}

	return 0;
}

int vibro_ref_store_read(uint8_t slot, struct vibro_ref_profile *out)
{
	const struct flash_area *fa;
	/* 2KB+ record — off the stack (matches vibro_capture/band_rms static-buffer fix). */
	static struct vibro_ref_record rec;
	int err;

	if (out == NULL || slot >= VIBRO_REF_STORE_SLOTS) {
		return -EINVAL;
	}

	err = flash_area_open(VIBRO_REF_PARTITION_ID, &fa);
	if (err != 0) {
		return err;
	}

	err = flash_area_read(fa, slot_offset(slot), &rec, sizeof(rec));
	flash_area_close(fa);
	if (err != 0) {
		return err;
	}

	if (rec.magic != VIBRO_REF_REC_MAGIC || rec.crc32 != rec_crc(&rec)) {
		return -ENOENT;
	}

	memset(out, 0, sizeof(*out));
	memcpy(out->name, rec.name, sizeof(out->name));
	out->name[VIBRO_REF_NAME_MAX - 1] = '\0';
	out->created_unix = rec.created_unix;
	out->updated_unix = rec.updated_unix;
	out->duration_ms = rec.duration_ms;
	out->sample_hz = rec.sample_hz;
	out->mag_len = MIN(rec.mag_len, VIBRO_REF_MAG_MAX);
	out->rms = rec.rms;
	out->peak = rec.peak;
	out->band_valid = rec.band_valid != 0U;
	memcpy(out->band_rms, rec.band_rms, sizeof(out->band_rms));
	memcpy(out->mag, rec.mag, out->mag_len * sizeof(float));
	return 0;
}

bool vibro_ref_store_valid(uint8_t slot)
{
	const struct flash_area *fa;
	uint32_t magic = 0;
	int err;

	if (slot >= VIBRO_REF_STORE_SLOTS) {
		return false;
	}

	err = flash_area_open(VIBRO_REF_PARTITION_ID, &fa);
	if (err != 0) {
		return false;
	}
	err = flash_area_read(fa, slot_offset(slot), &magic, sizeof(magic));
	flash_area_close(fa);
	return err == 0 && magic == VIBRO_REF_REC_MAGIC;
}

int vibro_ref_store_write(uint8_t slot, const struct vibro_ref_profile *prof)
{
	const struct flash_area *fa;
	/* Off the stack — see vibro_ref_store_read(). */
	static struct vibro_ref_record rec;
	int err;

	if (prof == NULL || slot >= VIBRO_REF_STORE_SLOTS) {
		return -EINVAL;
	}

	memset(&rec, 0, sizeof(rec));
	rec.magic = VIBRO_REF_REC_MAGIC;
	rec.version = VIBRO_REF_VERSION;
	rec.slot = slot;
	rec.band_valid = prof->band_valid ? 1U : 0U;
	snprintf(rec.name, sizeof(rec.name), "%s", prof->name);
	rec.created_unix = prof->created_unix;
	rec.updated_unix = prof->updated_unix;
	rec.duration_ms = prof->duration_ms;
	rec.sample_hz = prof->sample_hz;
	rec.mag_len = MIN(prof->mag_len, VIBRO_REF_MAG_MAX);
	rec.rms = prof->rms;
	rec.peak = prof->peak;
	memcpy(rec.band_rms, prof->band_rms, sizeof(rec.band_rms));
	memcpy(rec.mag, prof->mag, rec.mag_len * sizeof(float));
	rec.crc32 = rec_crc(&rec);

	err = flash_area_open(VIBRO_REF_PARTITION_ID, &fa);
	if (err != 0) {
		return err;
	}

	err = flash_area_erase(fa, slot_offset(slot), VIBRO_REF_SLOT_BYTES);
	if (err == 0) {
		err = flash_area_write(fa, slot_offset(slot), &rec, sizeof(rec));
	}
	flash_area_close(fa);
	if (err != 0) {
		LOG_ERR("ref store write slot=%u failed (%d)", slot, err);
	} else {
		LOG_INF("ref store slot=%u saved name=%s dur=%ums mag_len=%u", slot, rec.name,
			rec.duration_ms, rec.mag_len);
	}
	return err;
}

int vibro_ref_store_delete(uint8_t slot)
{
	const struct flash_area *fa;
	int err;

	if (slot >= VIBRO_REF_STORE_SLOTS) {
		return -EINVAL;
	}

	err = flash_area_open(VIBRO_REF_PARTITION_ID, &fa);
	if (err != 0) {
		return err;
	}
	err = flash_area_erase(fa, slot_offset(slot), VIBRO_REF_SLOT_BYTES);
	flash_area_close(fa);

	if (err == 0 && g_hdr.active_slot == (int8_t)slot) {
		(void)vibro_ref_store_set_active(-1);
	}
	return err;
}

int vibro_ref_store_set_active(int8_t slot)
{
	if (slot >= 0 && (uint8_t)slot >= VIBRO_REF_STORE_SLOTS) {
		return -EINVAL;
	}
	g_hdr.active_slot = slot;
	return write_header();
}

int8_t vibro_ref_store_active_slot(void)
{
	return g_loaded ? g_hdr.active_slot : -1;
}

int vibro_ref_store_init(void)
{
	int err = read_header();

	g_loaded = (err == 0);
	if (g_loaded) {
		uint8_t valid_count = 0;

		for (uint8_t s = 0; s < VIBRO_REF_STORE_SLOTS; s++) {
			if (vibro_ref_store_valid(s)) {
				valid_count++;
			}
		}
		LOG_INF("vibro ref store ready (slots=%u valid=%u active=%d)",
			VIBRO_REF_STORE_SLOTS, valid_count, g_hdr.active_slot);
	} else {
		LOG_WRN("vibro ref store init failed (%d)", err);
	}
	return err;
}

int vibro_ref_store_list_json(char *buf, size_t len)
{
	int n;

	if (buf == NULL || len == 0U) {
		return -EINVAL;
	}

	n = snprintf(buf, len, "{\"active\":%d,\"slots\":[", (int)vibro_ref_store_active_slot());
	if (n <= 0 || (size_t)n >= len) {
		return -ENOMEM;
	}

	for (uint8_t s = 0; s < VIBRO_REF_STORE_SLOTS; s++) {
		struct vibro_ref_profile prof;
		bool valid = vibro_ref_store_read(s, &prof) == 0;

		n += snprintf(buf + n, len - (size_t)n, "%s{\"slot\":%u,\"valid\":%u", s ? "," : "",
			      s, valid ? 1U : 0U);
		if (n <= 0 || (size_t)n >= len) {
			return -ENOMEM;
		}
		if (valid) {
			n += snprintf(buf + n, len - (size_t)n,
				      ",\"name\":\"%s\",\"dur_ms\":%u,\"hz\":%.1f,\"rms\":%.3f",
				      prof.name, prof.duration_ms, (double)prof.sample_hz,
				      (double)prof.rms);
			if (n <= 0 || (size_t)n >= len) {
				return -ENOMEM;
			}
		}
		n += snprintf(buf + n, len - (size_t)n, "}");
		if (n <= 0 || (size_t)n >= len) {
			return -ENOMEM;
		}
	}

	if ((size_t)n + 2U >= len) {
		return -ENOMEM;
	}
	n += snprintf(buf + n, len - (size_t)n, "]}");
	return n;
}
