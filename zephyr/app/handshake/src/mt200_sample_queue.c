#include "mt200_sample_queue.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(mt200_q, LOG_LEVEL_INF);

#define LIVE_MIN_INTERVAL_MS 1000U

/* PSRAM ring — offer() runs from BT GATT notify (BT RX WQ). Never use k_mutex
 * there (K_FOREVER can deadlock the BT stack). Spinlock only. */
static struct mt200_sample g_ring[MT200_SAMPLE_QUEUE_CAP] __attribute__((section(".ext_ram.bss")));

static uint16_t g_head; /* next write */
static uint16_t g_tail; /* next read */
static uint16_t g_count;
static struct k_spinlock g_lock;
static uint32_t g_last_hash;
static uint32_t g_last_offer_ms;
static uint32_t g_last_seq;
static bool g_inited;

static uint32_t hash_telem(const struct mt200_telem *t)
{
	uint32_t h = 2166136261u;

	h = (h ^ t->hr) * 16777619u;
	h = (h ^ t->spo2) * 16777619u;
	h = (h ^ t->bat_pct) * 16777619u;
	h = (h ^ t->flags) * 16777619u;
	h = (h ^ (t->steps & 0xffu)) * 16777619u;
	h = (h ^ ((t->steps >> 8) & 0xffu)) * 16777619u;
	h = (h ^ ((t->steps >> 16) & 0xffu)) * 16777619u;
	h = (h ^ ((t->steps >> 24) & 0xffu)) * 16777619u;
	h = (h ^ (t->kcal_x10 & 0xffu)) * 16777619u;
	h = (h ^ ((t->kcal_x10 >> 8) & 0xffu)) * 16777619u;
	h = (h ^ (t->dist_mm & 0xffu)) * 16777619u;
	h = (h ^ ((t->dist_mm >> 8) & 0xffu)) * 16777619u;
	return h;
}

void mt200_sample_queue_init(void)
{
	k_spinlock_key_t key;

	if (g_inited) {
		return;
	}
	key = k_spin_lock(&g_lock);
	g_head = 0;
	g_tail = 0;
	g_count = 0;
	g_last_hash = 0;
	g_last_offer_ms = 0;
	g_last_seq = 0;
	g_inited = true;
	k_spin_unlock(&g_lock, key);
	LOG_INF("mt200 sample queue ready cap=%u (spinlock)", MT200_SAMPLE_QUEUE_CAP);
}

bool mt200_sample_queue_offer_live(const struct mt200_telem *t, uint32_t wall_ms)
{
	struct mt200_sample s;
	uint32_t now;
	uint32_t h;
	k_spinlock_key_t key;

	if (!g_inited || t == NULL || t->flags == 0U) {
		return false;
	}

	now = k_uptime_get_32();
	h = hash_telem(t);

	key = k_spin_lock(&g_lock);
	if (h == g_last_hash) {
		k_spin_unlock(&g_lock, key);
		return false;
	}
	if (g_last_offer_ms != 0U && (now - g_last_offer_ms) < LIVE_MIN_INTERVAL_MS) {
		k_spin_unlock(&g_lock, key);
		return false;
	}

	memset(&s, 0, sizeof(s));
	s.esp_seq = t->seq;
	s.wall_ms = wall_ms;
	s.content_hash = h;
	s.hr = t->hr;
	s.spo2 = t->spo2;
	s.bat_pct = t->bat_pct;
	s.flags = t->flags;
	s.steps = t->steps;
	s.kcal_x10 = t->kcal_x10;
	s.dist_mm = t->dist_mm;
	s.rssi = t->rssi;

	if (g_count == MT200_SAMPLE_QUEUE_CAP) {
		g_tail = (uint16_t)((g_tail + 1U) % MT200_SAMPLE_QUEUE_CAP);
		g_count--;
	}
	g_ring[g_head] = s;
	g_head = (uint16_t)((g_head + 1U) % MT200_SAMPLE_QUEUE_CAP);
	g_count++;
	g_last_hash = h;
	g_last_offer_ms = now;
	g_last_seq = s.esp_seq;
	k_spin_unlock(&g_lock, key);
	return true;
}

uint16_t mt200_sample_queue_pending(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_lock);
	const uint16_t n = g_count;

	k_spin_unlock(&g_lock, key);
	return n;
}

uint16_t mt200_sample_queue_capacity(void)
{
	return MT200_SAMPLE_QUEUE_CAP;
}

bool mt200_sample_queue_pop(struct mt200_sample *out)
{
	bool ok = false;
	k_spinlock_key_t key;

	if (out == NULL) {
		return false;
	}
	key = k_spin_lock(&g_lock);
	if (g_count > 0U) {
		*out = g_ring[g_tail];
		g_tail = (uint16_t)((g_tail + 1U) % MT200_SAMPLE_QUEUE_CAP);
		g_count--;
		ok = true;
	}
	k_spin_unlock(&g_lock, key);
	return ok;
}

bool mt200_sample_queue_peek(struct mt200_sample *out)
{
	bool ok = false;
	k_spinlock_key_t key;

	if (out == NULL) {
		return false;
	}
	key = k_spin_lock(&g_lock);
	if (g_count > 0U) {
		*out = g_ring[g_tail];
		ok = true;
	}
	k_spin_unlock(&g_lock, key);
	return ok;
}

uint32_t mt200_sample_queue_last_seq(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_lock);
	const uint32_t seq = g_last_seq;

	k_spin_unlock(&g_lock, key);
	return seq;
}
