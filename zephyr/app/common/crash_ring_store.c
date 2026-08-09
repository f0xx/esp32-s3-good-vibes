/*
 * Crash ring — compact records in dedicated flash (step 3b, survives months offline).
 */

#include "crash_ring_store.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/drivers/flash.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>

LOG_MODULE_REGISTER(crash_ring, LOG_LEVEL_INF);

#define CRASH_RING_PARTITION   scratch_partition
#define CRASH_RING_PARTITION_ID FIXED_PARTITION_ID(CRASH_RING_PARTITION)

#if !FIXED_PARTITION_EXISTS(CRASH_RING_PARTITION)
#error "crash-ring flash partition required"
#endif

#define CRASH_HDR_MAGIC  0x43525348U /* CRSH */
#define CRASH_REC_MAGIC  0x43525352U /* CRSR */
#define CRASH_FLAG_PENDING 0x01U

#define CRASH_RING_FLASH_BYTES 4096U

#define CRASH_RECORD_SIZE 256U
#define CRASH_HEADER_SIZE 32U

struct crash_ring_header {
	uint32_t magic;
	uint32_t write_idx;
	uint32_t count;
	uint32_t next_seq;
	uint32_t crc32;
	uint32_t reserved[3];
} __packed;

struct crash_ring_record {
	uint32_t magic;
	uint32_t seq;
	uint32_t uptime_ms;
	uint32_t pc;
	uint32_t exccause;
	uint32_t excvaddr;
	uint8_t reset_reason;
	uint8_t flags;
	uint8_t bt_count;
	uint8_t pad0;
	uint16_t dump_size;
	uint16_t bat_mv;
	uint8_t bat_pct;
	uint8_t render_hz;
	uint8_t imu_hz;
	uint8_t power_profile;
	char reason[16];
	char fw_version[24];
	uint32_t backtrace[CRASH_REPORT_BACKTRACE_MAX];
	uint32_t crc32;
} __packed;

static struct crash_ring_header g_hdr;
static bool g_loaded;

static int format_ring(void);
static int write_header(void);

static uint32_t hdr_crc(const struct crash_ring_header *hdr)
{
	return crc32_ieee((const uint8_t *)hdr, offsetof(struct crash_ring_header, crc32));
}

static uint32_t rec_crc(const struct crash_ring_record *rec)
{
	return crc32_ieee((const uint8_t *)rec, offsetof(struct crash_ring_record, crc32));
}

static int read_header(void)
{
	const struct flash_area *fa;
	int err;

	err = flash_area_open(CRASH_RING_PARTITION_ID, &fa);
	if (err != 0) {
		return err;
	}

	err = flash_area_read(fa, 0, &g_hdr, sizeof(g_hdr));
	flash_area_close(fa);
	if (err != 0) {
		return err;
	}

	if (g_hdr.magic != CRASH_HDR_MAGIC || g_hdr.crc32 != hdr_crc(&g_hdr)) {
		memset(&g_hdr, 0, sizeof(g_hdr));
		g_hdr.magic = CRASH_HDR_MAGIC;
		g_hdr.next_seq = 1;
		err = format_ring();
		if (err != 0) {
			return err;
		}
		return write_header();
	}

	return 0;
}

static int format_ring(void)
{
	const struct flash_area *fa;
	int err;

	err = flash_area_open(CRASH_RING_PARTITION_ID, &fa);
	if (err != 0) {
		return err;
	}

	err = flash_area_erase(fa, 0, CRASH_RING_FLASH_BYTES);
	if (err != 0) {
		LOG_ERR("crash ring erase failed (%d) off=0x%x size=%u", err,
			(unsigned)fa->fa_off, (unsigned)CRASH_RING_FLASH_BYTES);
	}
	flash_area_close(fa);
	return err;
}

static int write_header(void)
{
	const struct flash_area *fa;
	int err;

	g_hdr.crc32 = hdr_crc(&g_hdr);
	err = flash_area_open(CRASH_RING_PARTITION_ID, &fa);
	if (err != 0) {
		return err;
	}

	err = flash_area_write(fa, 0, &g_hdr, sizeof(g_hdr));
	if (err != 0) {
		flash_area_close(fa);
		return format_ring();
	}
	flash_area_close(fa);
	return 0;
}

static int read_record(uint8_t slot, struct crash_ring_record *out)
{
	const struct flash_area *fa;
	off_t off;
	int err;

	if (out == NULL || slot >= CRASH_RING_SLOTS) {
		return -EINVAL;
	}

	err = flash_area_open(CRASH_RING_PARTITION_ID, &fa);
	if (err != 0) {
		return err;
	}

	off = (off_t)(CRASH_HEADER_SIZE + (size_t)slot * CRASH_RECORD_SIZE);
	err = flash_area_read(fa, off, out, sizeof(*out));
	flash_area_close(fa);
	if (err != 0) {
		return err;
	}

	if (out->magic != CRASH_REC_MAGIC || out->crc32 != rec_crc(out)) {
		return -ENOENT;
	}

	return 0;
}

static int write_record(uint8_t slot, const struct crash_ring_record *rec)
{
	const struct flash_area *fa;
	off_t off;
	int err;

	err = flash_area_open(CRASH_RING_PARTITION_ID, &fa);
	if (err != 0) {
		return err;
	}

	off = (off_t)(CRASH_HEADER_SIZE + (size_t)slot * CRASH_RECORD_SIZE);
	/* In-place write — slot area is pre-erased on ring format; avoid sub-sector erase. */
	err = flash_area_write(fa, off, rec, sizeof(*rec));
	flash_area_close(fa);
	return err;
}

int crash_ring_init(void)
{
	int err;

	if (g_loaded) {
		return 0;
	}

	err = read_header();

	g_loaded = (err == 0);
	if (g_loaded) {
		LOG_INF("crash ring ready (slots=%u pending=%u)", CRASH_RING_SLOTS,
			crash_ring_pending_count());
	} else {
		LOG_WRN("crash ring init failed (%d)", err);
	}
	return err;
}

int crash_ring_append(const struct crash_report_info *info, const struct crash_ring_telemetry *tel)
{
	struct crash_ring_record rec;
	uint8_t slot;

	if (!g_loaded || info == NULL) {
		return -EINVAL;
	}

	memset(&rec, 0, sizeof(rec));
	rec.magic = CRASH_REC_MAGIC;
	rec.seq = g_hdr.next_seq++;
	rec.uptime_ms = info->uptime_ms;
	rec.pc = info->pc;
	rec.exccause = info->exccause;
	rec.excvaddr = info->excvaddr;
	rec.reset_reason = info->reset_reason;
	rec.flags = CRASH_FLAG_PENDING;
	rec.dump_size = (uint16_t)MIN(info->dump_size, 0xFFFFU);
	rec.bt_count = MIN(info->backtrace_count, CRASH_REPORT_BACKTRACE_MAX);
	snprintf(rec.reason, sizeof(rec.reason), "%s",
		 info->reason != NULL ? info->reason : "unknown");
	snprintf(rec.fw_version, sizeof(rec.fw_version), "%s", info->fw_version);
	for (uint8_t i = 0; i < rec.bt_count; i++) {
		rec.backtrace[i] = info->backtrace[i];
	}
	if (tel != NULL) {
		rec.render_hz = tel->render_hz;
		rec.imu_hz = tel->imu_hz;
		rec.bat_mv = tel->bat_mv;
		rec.bat_pct = tel->bat_pct;
		rec.power_profile = tel->power_profile;
	}
	rec.crc32 = rec_crc(&rec);

	slot = (uint8_t)(g_hdr.write_idx % CRASH_RING_SLOTS);

	{
		struct crash_ring_record existing;

		if (read_record(slot, &existing) == 0) {
			if (format_ring() != 0) {
				return -EIO;
			}
		}
	}

	g_hdr.write_idx = (g_hdr.write_idx + 1U) % CRASH_RING_SLOTS;
	if (g_hdr.count < CRASH_RING_SLOTS) {
		g_hdr.count++;
	}

	if (write_record(slot, &rec) != 0 || write_header() != 0) {
		LOG_ERR("crash ring append failed slot=%u seq=%u", slot, rec.seq);
		return -EIO;
	}

	LOG_WRN("crash ring slot=%u seq=%u pc=0x%08x reason=%s", slot, rec.seq, rec.pc, rec.reason);
	return (int)slot;
}

uint8_t crash_ring_pending_count(void)
{
	uint8_t pending = 0;

	if (!g_loaded) {
		return 0;
	}

	for (uint8_t s = 0; s < CRASH_RING_SLOTS; s++) {
		struct crash_ring_record rec;

		if (read_record(s, &rec) == 0 && (rec.flags & CRASH_FLAG_PENDING) != 0U) {
			pending++;
		}
	}
	return pending;
}

bool crash_ring_slot_valid(uint8_t slot)
{
	struct crash_ring_record rec;

	return g_loaded && read_record(slot, &rec) == 0;
}

bool crash_ring_slot_pending(uint8_t slot)
{
	struct crash_ring_record rec;

	if (!g_loaded || read_record(slot, &rec) != 0) {
		return false;
	}
	return (rec.flags & CRASH_FLAG_PENDING) != 0U;
}

static int rec_to_info_json(const struct crash_ring_record *rec, uint8_t slot, char *buf, size_t len)
{
	int n;

	n = snprintf(buf, len,
		     "{\"pending\":1,\"slot\":%u,\"seq\":%u,\"size\":%u,\"pc\":%u,\"exccause\":%u,"
		     "\"excvaddr\":%u,\"reason\":\"%s\",\"fw\":\"%s\",\"reset\":%u,\"uptime\":%u,"
		     "\"bt\":[",
		     slot, rec->seq, rec->dump_size, rec->pc, rec->exccause, rec->excvaddr,
		     rec->reason, rec->fw_version, rec->reset_reason, rec->uptime_ms);
	if (n <= 0 || (size_t)n >= len) {
		return -ENOMEM;
	}

	for (uint8_t i = 0; i < rec->bt_count && (size_t)n < len - 24U; i++) {
		n += snprintf(buf + n, len - (size_t)n, "%s%u", i ? "," : "", rec->backtrace[i]);
	}

	n += snprintf(buf + n, len - (size_t)n,
		      "],\"detail\":{\"render_hz\":%u,\"imu_hz\":%u,\"bat_mv\":%u,\"bat_pct\":%u,"
		      "\"power_profile\":%u,\"dump_size\":%u}}",
		      rec->render_hz, rec->imu_hz, rec->bat_mv, rec->bat_pct, rec->power_profile,
		      rec->dump_size);
	return n > 0 ? n : -ENOMEM;
}

int crash_ring_first_pending_slot(void)
{
	for (uint8_t s = 0; s < CRASH_RING_SLOTS; s++) {
		if (crash_ring_slot_pending(s)) {
			return (int)s;
		}
	}
	return -1;
}

int crash_ring_next_pending_slot(uint8_t after)
{
	for (uint8_t s = (uint8_t)(after + 1U); s < CRASH_RING_SLOTS; s++) {
		if (crash_ring_slot_pending(s)) {
			return (int)s;
		}
	}
	return -1;
}

int crash_ring_list_json(char *buf, size_t len)
{
	int n = 0;
	uint8_t pending = crash_ring_pending_count();

	if (buf == NULL || len == 0U) {
		return -EINVAL;
	}

	n = snprintf(buf, len, "{\"pending\":%u,\"count\":%u,\"slots\":[", pending, g_hdr.count);
	if (n <= 0 || (size_t)n >= len) {
		return -ENOMEM;
	}

	bool first = true;
	for (uint8_t s = 0; s < CRASH_RING_SLOTS; s++) {
		struct crash_ring_record rec;

		if (read_record(s, &rec) != 0 || (rec.flags & CRASH_FLAG_PENDING) == 0U) {
			continue;
		}
		n += snprintf(buf + n, len - (size_t)n,
			      "%s{\"slot\":%u,\"seq\":%u,\"reason\":\"%s\",\"pc\":%u,\"uptime\":%u}",
			      first ? "" : ",", s, rec.seq, rec.reason, rec.pc, rec.uptime_ms);
		first = false;
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

int crash_ring_info_json(uint8_t slot, char *buf, size_t len)
{
	struct crash_ring_record rec;
	int err;

	err = read_record(slot, &rec);
	if (err != 0) {
		snprintf(buf, len, "{\"pending\":0,\"slot\":%u}", slot);
		return err;
	}
	return rec_to_info_json(&rec, slot, buf, len);
}

int crash_ring_clear_slot(uint8_t slot)
{
	struct crash_ring_record rec;

	if (read_record(slot, &rec) != 0) {
		return -ENOENT;
	}

	rec.flags &= ~CRASH_FLAG_PENDING;
	rec.crc32 = rec_crc(&rec);
	return write_record(slot, &rec);
}

void crash_ring_clear_all(void)
{
	for (uint8_t s = 0; s < CRASH_RING_SLOTS; s++) {
		(void)crash_ring_clear_slot(s);
	}
}
