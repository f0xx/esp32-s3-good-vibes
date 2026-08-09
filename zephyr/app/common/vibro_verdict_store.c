#include "vibro_verdict_store.h"

#include <stddef.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>

LOG_MODULE_REGISTER(vibro_spool, LOG_LEVEL_INF);

#define VERDICT_SPOOL_PARTITION   scratch_partition
#define VERDICT_SPOOL_PARTITION_ID FIXED_PARTITION_ID(VERDICT_SPOOL_PARTITION)

#if !FIXED_PARTITION_EXISTS(VERDICT_SPOOL_PARTITION)
#error "verdict spool requires scratch partition"
#endif

#define VERDICT_SPOOL_FLASH_OFF 4096U
#define VERDICT_SPOOL_FLASH_BYTES 4096U

#define VERDICT_HDR_MAGIC 0x56445248U /* VDRH */
#define VERDICT_REC_MAGIC 0x56445252U /* VDRR */
#define VERDICT_FLAG_PENDING 0x01U

#define VERDICT_HEADER_SIZE 32U
#define VERDICT_RECORD_SIZE 80U

struct verdict_spool_header {
	uint32_t magic;
	uint32_t write_idx;
	uint32_t count;
	uint32_t next_seq;
	uint32_t crc32;
	uint32_t reserved[3];
} __packed;

struct verdict_spool_record {
	uint32_t magic;
	uint32_t seq;
	uint32_t uptime_ms;
	uint8_t level;
	uint8_t flags;
	uint8_t has_reference;
	uint8_t band_valid;
	float rms_g;
	float peak_g;
	float corr;
	float rms_delta;
	float crest;
	float zcr_hz;
	float hf_ratio;
	float band0;
	float band1;
	float band2;
	float band3;
	uint32_t crc32;
} __packed;

static struct verdict_spool_header g_hdr;
static bool g_loaded;

static uint32_t hdr_crc(const struct verdict_spool_header *hdr)
{
	return crc32_ieee((const uint8_t *)hdr, offsetof(struct verdict_spool_header, crc32));
}

static uint32_t rec_crc(const struct verdict_spool_record *rec)
{
	return crc32_ieee((const uint8_t *)rec, offsetof(struct verdict_spool_record, crc32));
}

static int format_spool(void)
{
	const struct flash_area *fa;
	int err;

	memset(&g_hdr, 0, sizeof(g_hdr));
	g_hdr.magic = VERDICT_HDR_MAGIC;
	g_hdr.next_seq = 1U;
	g_hdr.crc32 = hdr_crc(&g_hdr);

	err = flash_area_open(VERDICT_SPOOL_PARTITION_ID, &fa);
	if (err != 0) {
		return err;
	}

	err = flash_area_erase(fa, VERDICT_SPOOL_FLASH_OFF, VERDICT_SPOOL_FLASH_BYTES);
	if (err != 0) {
		flash_area_close(fa);
		return err;
	}

	err = flash_area_write(fa, VERDICT_SPOOL_FLASH_OFF, &g_hdr, sizeof(g_hdr));
	flash_area_close(fa);
	return err;
}

static int write_header(void)
{
	const struct flash_area *fa;
	int err;

	g_hdr.crc32 = hdr_crc(&g_hdr);
	err = flash_area_open(VERDICT_SPOOL_PARTITION_ID, &fa);
	if (err != 0) {
		return err;
	}

	err = flash_area_write(fa, VERDICT_SPOOL_FLASH_OFF, &g_hdr, sizeof(g_hdr));
	flash_area_close(fa);
	if (err != 0) {
		return format_spool();
	}
	return 0;
}

static int read_header(void)
{
	const struct flash_area *fa;
	int err;

	err = flash_area_open(VERDICT_SPOOL_PARTITION_ID, &fa);
	if (err != 0) {
		return err;
	}

	err = flash_area_read(fa, VERDICT_SPOOL_FLASH_OFF, &g_hdr, sizeof(g_hdr));
	flash_area_close(fa);
	if (err != 0) {
		return err;
	}

	if (g_hdr.magic != VERDICT_HDR_MAGIC || g_hdr.crc32 != hdr_crc(&g_hdr)) {
		return format_spool();
	}

	return 0;
}

static int read_record(uint8_t slot, struct verdict_spool_record *out)
{
	const struct flash_area *fa;
	off_t off;
	int err;

	if (out == NULL || slot >= VERDICT_SPOOL_SLOTS) {
		return -EINVAL;
	}

	err = flash_area_open(VERDICT_SPOOL_PARTITION_ID, &fa);
	if (err != 0) {
		return err;
	}

	off = (off_t)(VERDICT_SPOOL_FLASH_OFF + VERDICT_HEADER_SIZE +
		      (size_t)slot * VERDICT_RECORD_SIZE);
	err = flash_area_read(fa, off, out, sizeof(*out));
	flash_area_close(fa);
	if (err != 0) {
		return err;
	}

	if (out->magic != VERDICT_REC_MAGIC || out->crc32 != rec_crc(out)) {
		return -ENOENT;
	}

	return 0;
}

static int write_record(uint8_t slot, const struct verdict_spool_record *rec)
{
	const struct flash_area *fa;
	off_t off;
	int err;

	err = flash_area_open(VERDICT_SPOOL_PARTITION_ID, &fa);
	if (err != 0) {
		return err;
	}

	off = (off_t)(VERDICT_SPOOL_FLASH_OFF + VERDICT_HEADER_SIZE +
		      (size_t)slot * VERDICT_RECORD_SIZE);
	err = flash_area_write(fa, off, rec, sizeof(*rec));
	flash_area_close(fa);
	return err;
}

int vibro_verdict_store_init(void)
{
	int err = read_header();

	g_loaded = (err == 0);
	if (g_loaded) {
		LOG_INF("verdict spool ready (slots=%u pending=%u)", VERDICT_SPOOL_SLOTS,
			vibro_verdict_store_pending_count());
	} else {
		LOG_WRN("verdict spool init failed (%d)", err);
	}
	return err;
}

uint32_t vibro_verdict_store_alloc_seq(void)
{
	uint32_t seq;

	if (!g_loaded) {
		return 0U;
	}

	seq = g_hdr.next_seq;
	if (seq == 0U) {
		seq = 1U;
	}
	g_hdr.next_seq = seq + 1U;
	(void)write_header();
	return seq;
}

int vibro_verdict_store_append(uint32_t seq, uint32_t uptime_ms,
			       const struct vibro_verdict *verdict,
			       const struct vibro_edge_features *edge,
			       const struct vibro_band_rms *bands)
{
	struct verdict_spool_record rec;
	uint8_t slot;

	if (!g_loaded || verdict == NULL || !verdict->valid || seq == 0U) {
		return -EINVAL;
	}

	memset(&rec, 0, sizeof(rec));
	rec.magic = VERDICT_REC_MAGIC;
	rec.seq = seq;
	rec.uptime_ms = uptime_ms;
	rec.level = (uint8_t)verdict->level;
	rec.flags = VERDICT_FLAG_PENDING;
	rec.has_reference = verdict->has_reference ? 1U : 0U;
	rec.rms_g = verdict->rms_g;
	rec.peak_g = verdict->peak_g;
	rec.corr = verdict->corr;
	rec.rms_delta = verdict->rms_delta;
	if (edge != NULL && edge->valid) {
		rec.crest = edge->crest;
		rec.zcr_hz = edge->zcr_hz;
		rec.hf_ratio = edge->hf_ratio;
	}
	if (bands != NULL && bands->valid) {
		rec.band_valid = 1U;
		rec.band0 = bands->bands[0];
		rec.band1 = bands->bands[1];
		rec.band2 = bands->bands[2];
		rec.band3 = bands->bands[3];
	}
	rec.crc32 = rec_crc(&rec);

	slot = (uint8_t)(g_hdr.write_idx % VERDICT_SPOOL_SLOTS);

	{
		struct verdict_spool_record existing;

		if (read_record(slot, &existing) == 0 && (existing.flags & VERDICT_FLAG_PENDING) != 0U) {
			if (format_spool() != 0) {
				return -EIO;
			}
		}
	}

	g_hdr.write_idx = (g_hdr.write_idx + 1U) % VERDICT_SPOOL_SLOTS;
	if (g_hdr.count < VERDICT_SPOOL_SLOTS) {
		g_hdr.count++;
	}
	if (seq >= g_hdr.next_seq) {
		g_hdr.next_seq = seq + 1U;
	}

	if (write_record(slot, &rec) != 0 || write_header() != 0) {
		LOG_ERR("verdict spool append failed slot=%u seq=%u", slot, seq);
		return -EIO;
	}

	LOG_INF("verdict spool slot=%u seq=%u level=%u", slot, seq, rec.level);
	return 0;
}

uint16_t vibro_verdict_store_pending_count(void)
{
	uint16_t pending = 0;

	if (!g_loaded) {
		return 0;
	}

	for (uint8_t s = 0; s < VERDICT_SPOOL_SLOTS; s++) {
		struct verdict_spool_record rec;

		if (read_record(s, &rec) == 0 && (rec.flags & VERDICT_FLAG_PENDING) != 0U) {
			pending++;
		}
	}
	return pending;
}

uint32_t vibro_verdict_store_last_seq(void)
{
	uint32_t max_seq = 0U;

	if (!g_loaded) {
		return 0U;
	}

	for (uint8_t s = 0; s < VERDICT_SPOOL_SLOTS; s++) {
		struct verdict_spool_record rec;

		if (read_record(s, &rec) == 0 && rec.seq > max_seq) {
			max_seq = rec.seq;
		}
	}
	return max_seq;
}

uint32_t vibro_verdict_store_first_pending_seq(void)
{
	uint32_t min_seq = 0U;

	if (!g_loaded) {
		return 0U;
	}

	for (uint8_t s = 0; s < VERDICT_SPOOL_SLOTS; s++) {
		struct verdict_spool_record rec;

		if (read_record(s, &rec) != 0) {
			continue;
		}
		if ((rec.flags & VERDICT_FLAG_PENDING) == 0U) {
			continue;
		}
		if (min_seq == 0U || rec.seq < min_seq) {
			min_seq = rec.seq;
		}
	}
	return min_seq;
}

bool vibro_verdict_store_ack(uint32_t seq)
{
	bool cleared = false;

	if (!g_loaded || seq == 0U) {
		return false;
	}

	for (uint8_t s = 0; s < VERDICT_SPOOL_SLOTS; s++) {
		struct verdict_spool_record rec;

		if (read_record(s, &rec) != 0) {
			continue;
		}
		if ((rec.flags & VERDICT_FLAG_PENDING) == 0U) {
			continue;
		}
		if (rec.seq > seq) {
			continue;
		}

		rec.flags &= ~VERDICT_FLAG_PENDING;
		rec.crc32 = rec_crc(&rec);
		if (write_record(s, &rec) == 0) {
			cleared = true;
			LOG_INF("verdict spool ack slot=%u seq=%u", s, rec.seq);
		}
	}

	if (cleared) {
		(void)write_header();
	}
	return cleared;
}
