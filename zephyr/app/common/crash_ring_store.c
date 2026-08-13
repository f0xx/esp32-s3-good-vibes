/*
 * Crash ring — compact records in dedicated flash (step 3b, survives months offline).
 */

#include "crash_ring_store.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(crash_ring, LOG_LEVEL_INF);

#define CRASH_RING_PARTITION   scratch_partition
#define CRASH_RING_PARTITION_ID FIXED_PARTITION_ID(CRASH_RING_PARTITION)

#if !FIXED_PARTITION_EXISTS(CRASH_RING_PARTITION)
#error "crash-ring flash partition required"
#endif

#define CRASH_HDR_MAGIC  0x43525348U /* CRSH */
#define CRASH_REC_MAGIC  0x43525352U /* CRSR */
#define CRASH_FLAG_PENDING 0x01U

#define CRASH_RING_SECTOR_BYTES 4096U
/*
 * scratch_partition (256KB) layout — DO NOT move any consumer's base offset without checking
 * all of these against each other, they all share this one partition:
 *   0        : crash_ring sector A (this file, 4KB)
 *   4096     : vibro_verdict_store (verdict spool, 4KB)
 *   8192     : vibro_ref_store header (4KB)
 *   12288    : vibro_ref_store slots, VIBRO_REF_STORE_SLOTS x 4KB (20KB) -> ends at 32768
 *   32768    : crash_ring sector B (this file, 4KB)  <-- picked here, not at a contiguous
 *              offset from sector A, specifically to land in free space past every other
 *              consumer instead of colliding with vibro_verdict_store at 4096.
 *   36864+   : free
 *
 * scratch_partition is far more than the 8KB (two sectors) this needs — so we can ping-pong the
 * ring across two independent sectors instead of squeezing everything into one. See
 * persist_ring()'s doc comment for why a single shared sector isn't safe against a reset
 * landing mid-erase/write.
 */
#define CRASH_RING_NUM_SECTORS   2U
static const off_t g_sector_base[CRASH_RING_NUM_SECTORS] = { 0, 32768 };

#define CRASH_RECORD_SIZE 256U
#define CRASH_HEADER_SIZE 32U

struct crash_ring_header {
	uint32_t magic;
	uint32_t write_idx;
	uint32_t count;
	uint32_t next_seq;
	uint32_t crc32;
	uint32_t generation;
	uint32_t reserved[2];
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
/* Sector currently holding the last known-good, fully-committed copy of the ring. Every
 * persist_ring() call writes to the *other* sector and only flips this after that write fully
 * succeeds — see persist_ring()'s doc comment. */
static uint8_t g_active_sector;

static void sync_next_seq_from_slots(void);

static uint32_t hdr_crc(const struct crash_ring_header *hdr)
{
	return crc32_ieee((const uint8_t *)hdr, offsetof(struct crash_ring_header, crc32));
}

static uint32_t rec_crc(const struct crash_ring_record *rec)
{
	return crc32_ieee((const uint8_t *)rec, offsetof(struct crash_ring_record, crc32));
}

static off_t sector_base(uint8_t sector)
{
	return g_sector_base[sector];
}

static int read_sector_header(uint8_t sector, struct crash_ring_header *out)
{
	const struct flash_area *fa;
	int err;

	err = flash_area_open(CRASH_RING_PARTITION_ID, &fa);
	if (err != 0) {
		return err;
	}
	err = flash_area_read(fa, sector_base(sector), out, sizeof(*out));
	flash_area_close(fa);
	if (err != 0) {
		return err;
	}
	if (out->magic != CRASH_HDR_MAGIC || out->crc32 != hdr_crc(out)) {
		return -ENOENT;
	}
	return 0;
}

static int read_record_from(uint8_t sector, uint8_t slot, struct crash_ring_record *out)
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

	off = sector_base(sector) + (off_t)(CRASH_HEADER_SIZE + (size_t)slot * CRASH_RECORD_SIZE);
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

static int read_record(uint8_t slot, struct crash_ring_record *out)
{
	return read_record_from(g_active_sector, slot, out);
}

/*
 * NOR flash can only clear bits (1->0) with a plain write; setting any bit back to 1 requires
 * erasing the whole sector first. A previous version of this code erased+rewrote the *same*
 * sector in place for every update. That was already fixed once (see git history) to always
 * snapshot every still-valid record before erasing, but it turned out a second, more subtle
 * hazard remained: if a reset lands *during* that erase or rewrite (observed on real hardware
 * as an intermittent hardware TG0WDT_SYS_RST — ESP-IDF's flash driver disables cache/interrupts
 * on both cores for the ~15-40ms erase, which occasionally collides with the BLE controller's
 * own tight connection-event timing while a link is active), the sector being rewritten is left
 * in a half-erased/half-written state with no valid fallback, wiping every crash record that
 * was in it — including ones that were never touched by this particular update.
 *
 * Fix: ping-pong between two independent 4KB sectors (scratch_partition has 256KB, so this
 * costs nothing). Every persist_ring() call writes a *complete* new copy (header + every still
 * valid record) into the *other* sector, and only flips g_active_sector to it after the whole
 * write succeeds. The old sector is never touched, so no matter when a reset strikes the new
 * sector's write, the old sector is still there, fully intact, and gets picked up as the newest
 * *valid* sector on the next boot (see read_header()'s generation comparison). Records are
 * written before the header, and the header is written last with a bumped `generation` — the
 * header's presence with a valid CRC is the "commit" signal that the whole sector is complete,
 * so a reset that only got partway through the records leaves the header invalid and that
 * sector is correctly ignored on boot.
 *
 * `override_slot`/`override_rec` let a record update (or clear) be folded into the same
 * ping-pong cycle as the header update it's paired with. `clear_pending_mask` (bit s = slot s)
 * additionally drops CRASH_FLAG_PENDING on any of those slots that still hold a valid record —
 * this is what lets a multi-slot "clear" collapse N slot clears into a single cycle instead
 * of N.
 */
static int persist_ring(int8_t override_slot, const struct crash_ring_record *override_rec,
			 uint32_t clear_pending_mask)
{
	const struct flash_area *fa;
	struct crash_ring_record recs[CRASH_RING_SLOTS];
	uint8_t target = (uint8_t)(1U - g_active_sector);
	int err;

	for (uint8_t s = 0; s < CRASH_RING_SLOTS; s++) {
		if (read_record(s, &recs[s]) != 0) {
			recs[s].magic = 0U;
		}
	}
	if (override_slot >= 0 && override_slot < (int8_t)CRASH_RING_SLOTS) {
		recs[override_slot] = *override_rec;
	}
	for (uint8_t s = 0; s < CRASH_RING_SLOTS; s++) {
		if ((clear_pending_mask & BIT(s)) != 0U && recs[s].magic == CRASH_REC_MAGIC) {
			recs[s].flags &= ~CRASH_FLAG_PENDING;
			recs[s].crc32 = rec_crc(&recs[s]);
		}
	}

	err = flash_area_open(CRASH_RING_PARTITION_ID, &fa);
	if (err != 0) {
		return err;
	}

	err = flash_area_erase(fa, sector_base(target), CRASH_RING_SECTOR_BYTES);
	if (err != 0) {
		LOG_ERR("crash ring erase failed sector=%u (%d)", target, err);
		flash_area_close(fa);
		return err;
	}

	/* Records first, header last (commit signal) — see doc comment above. */
	for (uint8_t s = 0; err == 0 && s < CRASH_RING_SLOTS; s++) {
		if (recs[s].magic != CRASH_REC_MAGIC) {
			continue;
		}
		off_t off = sector_base(target) +
			    (off_t)(CRASH_HEADER_SIZE + (size_t)s * CRASH_RECORD_SIZE);

		err = flash_area_write(fa, off, &recs[s], sizeof(recs[s]));
	}

	if (err == 0) {
		struct crash_ring_header hdr = g_hdr;

		hdr.generation = g_hdr.generation + 1U;
		hdr.crc32 = hdr_crc(&hdr);
		err = flash_area_write(fa, sector_base(target), &hdr, sizeof(hdr));
		if (err == 0) {
			g_hdr = hdr;
		}
	}

	flash_area_close(fa);
	if (err != 0) {
		LOG_ERR("crash ring persist failed sector=%u (%d)", target, err);
		return err;
	}

	g_active_sector = target;
	return 0;
}

static int write_header(void)
{
	return persist_ring(-1, NULL, 0U);
}

/* Picks whichever of the two sectors is valid (magic+CRC ok) with the higher generation —
 * that is always the most recently *fully committed* copy, per persist_ring()'s doc comment.
 * If neither is valid (first boot, or a very unlucky reset that hit both), starts fresh. */
static int read_header(void)
{
	struct crash_ring_header hdrs[CRASH_RING_NUM_SECTORS];
	bool valid[CRASH_RING_NUM_SECTORS];
	int best = -1;

	for (uint8_t s = 0; s < CRASH_RING_NUM_SECTORS; s++) {
		valid[s] = (read_sector_header(s, &hdrs[s]) == 0);
		if (valid[s] && (best < 0 || hdrs[s].generation > hdrs[best].generation)) {
			best = (int)s;
		}
	}

	if (best >= 0) {
		g_hdr = hdrs[best];
		g_active_sector = (uint8_t)best;
		return 0;
	}

	LOG_WRN("crash ring header invalid/corrupt on both sectors — starting fresh");
	memset(&g_hdr, 0, sizeof(g_hdr));
	g_hdr.magic = CRASH_HDR_MAGIC;
	g_hdr.next_seq = 1;
	g_active_sector = 1; /* so the first persist_ring() targets sector 0 */
	return write_header();
}

static void sync_next_seq_from_slots(void)
{
	uint32_t max_seq = 0;

	for (uint8_t s = 0; s < CRASH_RING_SLOTS; s++) {
		struct crash_ring_record rec;

		if (read_record(s, &rec) == 0 && rec.seq > max_seq) {
			max_seq = rec.seq;
		}
	}
	if (g_hdr.next_seq <= max_seq) {
		g_hdr.next_seq = max_seq + 1U;
		if (write_header() != 0) {
			LOG_WRN("crash ring next_seq bump failed");
		} else {
			LOG_INF("crash ring next_seq=%u (from slots)", g_hdr.next_seq);
		}
	}
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
		sync_next_seq_from_slots();
		LOG_INF("crash ring ready (slots=%u pending=%u sector=%u gen=%u)", CRASH_RING_SLOTS,
			crash_ring_pending_count(), g_active_sector, g_hdr.generation);
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

	/* No pre-check/reformat needed for a slot that already holds a record: persist_ring()
	 * always ping-pongs to a fresh sector with every still-valid record rewritten together,
	 * so overwriting the oldest slot when the ring wraps is safe and never touches the other
	 * slots' data. */
	slot = (uint8_t)(g_hdr.write_idx % CRASH_RING_SLOTS);

	g_hdr.write_idx = (g_hdr.write_idx + 1U) % CRASH_RING_SLOTS;
	if (g_hdr.count < CRASH_RING_SLOTS) {
		g_hdr.count++;
	}

	/* Header (write_idx/count/next_seq) and this record share one persist_ring() call so the
	 * whole append is a single ping-pong cycle instead of two. */
	if (persist_ring((int8_t)slot, &rec, 0U) != 0) {
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

/*
 * Shared by rec_to_info_json() (standalone single-slot read — BLE ATT attribute values are
 * hard-capped at 512B by the Bluetooth spec, so a single record can afford full detail) and
 * crash_ring_list_json()'s per-item loop (bulk multi-slot read, must budget bytes across up to
 * CRASH_RING_SLOTS records within that same 512B cap). `max_bt`/`include_detail` let the bulk
 * path shrink each entry so 2+ pending crashes still fit in one read instead of falling back to
 * per-slot write+read ping-pong for every slot.
 */
static int append_record_json(char *buf, size_t off, size_t len, uint8_t slot,
			       const struct crash_ring_record *rec, uint8_t max_bt,
			       bool include_detail)
{
	int n;
	uint8_t bt_count = rec->bt_count < max_bt ? rec->bt_count : max_bt;

	n = snprintf(buf + off, len - off,
		     "{\"pending\":1,\"slot\":%u,\"seq\":%u,\"size\":%u,\"pc\":%u,\"exccause\":%u,"
		     "\"excvaddr\":%u,\"reason\":\"%s\",\"fw\":\"%s\",\"reset\":%u,\"uptime\":%u,"
		     "\"bt\":[",
		     slot, rec->seq, rec->dump_size, rec->pc, rec->exccause, rec->excvaddr,
		     rec->reason, rec->fw_version, rec->reset_reason, rec->uptime_ms);
	if (n <= 0 || off + (size_t)n >= len) {
		return -ENOMEM;
	}
	off += (size_t)n;
	size_t total = (size_t)n;

	for (uint8_t i = 0; i < bt_count && off < len - 24U; i++) {
		n = snprintf(buf + off, len - off, "%s%u", i ? "," : "", rec->backtrace[i]);
		if (n <= 0) {
			break;
		}
		off += (size_t)n;
		total += (size_t)n;
	}

	if (include_detail) {
		n = snprintf(buf + off, len - off,
			     "],\"detail\":{\"render_hz\":%u,\"imu_hz\":%u,\"bat_mv\":%u,"
			     "\"bat_pct\":%u,\"power_profile\":%u,\"dump_size\":%u}}",
			     rec->render_hz, rec->imu_hz, rec->bat_mv, rec->bat_pct,
			     rec->power_profile, rec->dump_size);
	} else {
		n = snprintf(buf + off, len - off, "]}");
	}
	if (n <= 0) {
		return -ENOMEM;
	}
	total += (size_t)n;
	return (int)total;
}

static int rec_to_info_json(const struct crash_ring_record *rec, uint8_t slot, char *buf, size_t len)
{
	return append_record_json(buf, 0, len, slot, rec, rec->bt_count, true);
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

/* Emits full per-slot detail (same fields as rec_to_info_json) for every pending slot in one
 * shot, so the BLE client can fetch+upload+clear all pending crashes without any per-slot
 * write+read ping-pong — that ping-pong was the main source of "crash clear failed" /
 * long disconnect-reconnect cycles when several crashes were pending at once. */
int crash_ring_list_json(char *buf, size_t len)
{
	size_t off;
	int n = 0;
	uint8_t pending = crash_ring_pending_count();

	if (buf == NULL || len == 0U) {
		return -EINVAL;
	}

	n = snprintf(buf, len, "{\"pending\":%u,\"count\":%u,\"slots\":[", pending, g_hdr.count);
	if (n <= 0 || (size_t)n >= len) {
		return -ENOMEM;
	}
	off = (size_t)n;

	bool first = true;
	for (uint8_t s = 0; s < CRASH_RING_SLOTS; s++) {
		struct crash_ring_record rec;

		if (read_record(s, &rec) != 0 || (rec.flags & CRASH_FLAG_PENDING) == 0U) {
			continue;
		}
		if (!first) {
			if (off + 1U >= len) {
				return -ENOMEM;
			}
			buf[off++] = ',';
		}
		n = append_record_json(buf, off, len, s, &rec, 4U, false);
		if (n < 0) {
			/* Out of room for this slot's full detail — stop here; the client will
			 * pick up the remaining slot(s) on the next relay round. */
			if (!first) {
				off--; /* drop the trailing comma we just wrote */
			}
			break;
		}
		off += (size_t)n;
		first = false;
	}

	if (off + 2U >= len) {
		return -ENOMEM;
	}
	buf[off++] = ']';
	buf[off++] = '}';
	buf[off] = '\0';
	return (int)off;
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
	if (slot >= CRASH_RING_SLOTS || !crash_ring_slot_valid(slot)) {
		return -ENOENT;
	}
	return persist_ring(-1, NULL, BIT(slot));
}

/* Clears several slots' pending flags in ONE ping-pong cycle instead of one cycle per slot —
 * see persist_ring()'s doc comment for why chaining N separate cycles from a single BLE GATT
 * write used to risk losing data if a reset landed mid-cycle. */
int crash_ring_clear_slots(const uint8_t *slots, size_t n)
{
	uint32_t mask = 0U;

	if (slots == NULL) {
		return -EINVAL;
	}
	for (size_t i = 0; i < n; i++) {
		if (slots[i] < CRASH_RING_SLOTS) {
			mask |= BIT(slots[i]);
		}
	}
	if (mask == 0U) {
		return 0;
	}
	return persist_ring(-1, NULL, mask);
}

void crash_ring_clear_all(void)
{
	persist_ring(-1, NULL, BIT_MASK(CRASH_RING_SLOTS));
}
