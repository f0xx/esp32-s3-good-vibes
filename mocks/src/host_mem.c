#include "host_mem.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <zephyr/logging/log.h>

#include "host_debug.h"

LOG_MODULE_REGISTER(host_mem, LOG_LEVEL_INF);

#ifdef HOST_FLAT_HEAP_WRAP
void *__real_malloc(size_t size);
void *__real_calloc(size_t nmemb, size_t size);
void *__real_realloc(void *ptr, size_t size);
void __real_free(void *ptr);
#define LIBC_MALLOC __real_malloc
#define LIBC_CALLOC __real_calloc
#define LIBC_REALLOC __real_realloc
#define LIBC_FREE __real_free
#else
#define LIBC_MALLOC malloc
#define LIBC_CALLOC calloc
#define LIBC_REALLOC realloc
#define LIBC_FREE free
#endif


enum {
	BLK_MAGIC_USED = 0x484D5553u, /* 'HMUS' */
	BLK_MAGIC_FREE = 0x484D4652u, /* 'HMFR' */
	REDZONE = 8,
	HDR_ALIGN = 16,
};

struct host_blk {
	uint32_t magic;
	uint32_t flags; /* bit0 = psram region preference recorded */
	size_t size;    /* payload bytes */
	size_t capacity; /* payload + redzone + padding to next hdr */
	struct host_blk *next_free;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *g_arena;
static size_t g_arena_bytes;
static struct host_blk *g_free_list;
static bool g_inited;
static bool g_wanted = true;
static bool g_abort_on_fault = true;
static struct host_mem_stats g_stats;

static size_t align_up(size_t n, size_t a)
{
	return (n + a - 1u) & ~(a - 1u);
}

static void fault(const char *what, const void *ptr)
{
	HOST_LOG(HOST_LOG_ERR, "host_mem", "%s ptr=%p", what, ptr);
	fflush(stderr);
	if (g_abort_on_fault) {
		abort();
	}
}

static struct host_blk *hdr_from_ptr(void *ptr)
{
	return (struct host_blk *)((uint8_t *)ptr - sizeof(struct host_blk));
}

static void *payload_of(struct host_blk *b)
{
	return (uint8_t *)b + sizeof(struct host_blk);
}

static uint8_t *redzone_of(struct host_blk *b)
{
	return (uint8_t *)payload_of(b) + b->size;
}

static void paint_redzone(struct host_blk *b)
{
	memset(redzone_of(b), 0xA5, REDZONE);
}

static bool check_redzone(struct host_blk *b)
{
	const uint8_t *rz = redzone_of(b);

	for (int i = 0; i < REDZONE; i++) {
		if (rz[i] != 0xA5) {
			return false;
		}
	}
	return true;
}

static bool ptr_in_arena(const void *ptr)
{
	const uint8_t *p = ptr;

	return g_arena && p >= g_arena && p < g_arena + g_arena_bytes;
}

bool host_mem_contains(const void *ptr)
{
	return ptr_in_arena(ptr);
}

static void unlink_free(struct host_blk *b)
{
	struct host_blk **pp = &g_free_list;

	while (*pp) {
		if (*pp == b) {
			*pp = b->next_free;
			b->next_free = NULL;
			return;
		}
		pp = &(*pp)->next_free;
	}
}

static void insert_free(struct host_blk *b)
{
	b->magic = BLK_MAGIC_FREE;
	b->next_free = g_free_list;
	g_free_list = b;
	memset(payload_of(b), 0xDD, b->size);
}

static void coalesce(void)
{
	/* Address-order walk of free list is O(n^2); fine for host sim. */
	bool changed = true;

	while (changed) {
		changed = false;
		for (struct host_blk *a = g_free_list; a; a = a->next_free) {
			uint8_t *a_end = (uint8_t *)a + sizeof(struct host_blk) + a->capacity;
			for (struct host_blk *b = g_free_list; b; b = b->next_free) {
				if (b == a) {
					continue;
				}
				if ((uint8_t *)b == a_end) {
					unlink_free(b);
					a->capacity += sizeof(struct host_blk) + b->capacity;
					a->size = a->capacity > REDZONE ? a->capacity - REDZONE : 0;
					changed = true;
					break;
				}
			}
			if (changed) {
				break;
			}
		}
	}
}

static struct host_blk *find_fit(size_t need_cap, bool prefer_psram)
{
	struct host_blk *best = NULL;
	const uint8_t *dram_end = g_arena + HOST_MEM_DRAM_BYTES;

	for (struct host_blk *b = g_free_list; b; b = b->next_free) {
		if (b->capacity < need_cap) {
			continue;
		}
		const bool in_psram = (uint8_t *)b >= dram_end;
		if (prefer_psram != in_psram && best) {
			continue;
		}
		if (!best || b->capacity < best->capacity) {
			best = b;
			if (prefer_psram == in_psram && b->capacity == need_cap) {
				break;
			}
		}
	}
	if (!best) {
		/* Fall back to any region. */
		for (struct host_blk *b = g_free_list; b; b = b->next_free) {
			if (b->capacity >= need_cap && (!best || b->capacity < best->capacity)) {
				best = b;
			}
		}
	}
	return best;
}

static void *alloc_from_free(size_t size, bool prefer_psram)
{
	const size_t payload = align_up(size == 0 ? 1 : size, HOST_MEM_ALIGN);
	const size_t need_cap = align_up(payload + REDZONE, HDR_ALIGN);
	struct host_blk *b = find_fit(need_cap, prefer_psram);

	if (!b) {
		g_stats.fails++;
		return NULL;
	}

	unlink_free(b);

	/* Split if leftover can hold another header + min payload. */
	const size_t remain = b->capacity - need_cap;
	if (remain >= sizeof(struct host_blk) + HOST_MEM_ALIGN + REDZONE + HDR_ALIGN) {
		struct host_blk *tail =
			(struct host_blk *)((uint8_t *)b + sizeof(struct host_blk) + need_cap);
		tail->magic = BLK_MAGIC_FREE;
		tail->flags = 0;
		tail->capacity = remain - sizeof(struct host_blk);
		tail->size = tail->capacity > REDZONE ? tail->capacity - REDZONE : 0;
		tail->next_free = NULL;
		insert_free(tail);
		b->capacity = need_cap;
	}

	b->magic = BLK_MAGIC_USED;
	b->flags = prefer_psram ? 1u : 0u;
	b->size = payload;
	b->next_free = NULL;
	paint_redzone(b);
	memset(payload_of(b), 0xCD, payload);

	g_stats.allocs++;
	g_stats.used_bytes += b->capacity + sizeof(struct host_blk);
	if (g_stats.used_bytes > g_stats.peak_used) {
		g_stats.peak_used = g_stats.used_bytes;
	}
	g_stats.free_bytes = g_arena_bytes > g_stats.used_bytes ?
				     g_arena_bytes - g_stats.used_bytes :
				     0;

	return payload_of(b);
}

void host_mem_set_wanted(bool want)
{
	g_wanted = want;
}

bool host_mem_enabled(void)
{
	return g_inited && g_wanted;
}

int host_mem_init(void)
{
	const char *no = getenv("HOST_NO_FLAT_HEAP");

	pthread_mutex_lock(&g_lock);
	if (g_inited) {
		pthread_mutex_unlock(&g_lock);
		return 0;
	}
	if (no && no[0] && strcmp(no, "0") != 0) {
		g_wanted = false;
	}
	if (!g_wanted) {
		LOG_INF("flat heap disabled (HOST_NO_FLAT_HEAP / --no-flat-heap)");
		g_inited = true;
		pthread_mutex_unlock(&g_lock);
		return 0;
	}

	g_arena_bytes = HOST_MEM_TOTAL_BYTES;
	g_arena = mmap(NULL, g_arena_bytes, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (g_arena == MAP_FAILED) {
		g_arena = NULL;
		LOG_ERR("mmap arena %zu failed errno=%d — falling back to libc", g_arena_bytes,
			errno);
		g_wanted = false;
		g_inited = true;
		pthread_mutex_unlock(&g_lock);
		return -1;
	}

	memset(&g_stats, 0, sizeof(g_stats));
	g_stats.arena_bytes = g_arena_bytes;
	g_stats.dram_bytes = HOST_MEM_DRAM_BYTES;
	g_stats.psram_bytes = HOST_MEM_PSRAM_BYTES;
	g_stats.free_bytes = g_arena_bytes;

	struct host_blk *root = (struct host_blk *)g_arena;
	root->magic = BLK_MAGIC_FREE;
	root->flags = 0;
	root->capacity = g_arena_bytes - sizeof(struct host_blk);
	root->size = root->capacity > REDZONE ? root->capacity - REDZONE : 0;
	root->next_free = NULL;
	g_free_list = NULL;
	insert_free(root);

	g_inited = true;
	LOG_INF("flat heap arena=%zu (DRAM %u + PSRAM %u) base=%p align=%u", g_arena_bytes,
		(unsigned)HOST_MEM_DRAM_BYTES, (unsigned)HOST_MEM_PSRAM_BYTES, (void *)g_arena,
		(unsigned)HOST_MEM_ALIGN);
	pthread_mutex_unlock(&g_lock);
	return 0;
}

void host_mem_shutdown(void)
{
	pthread_mutex_lock(&g_lock);
	if (g_arena) {
		munmap(g_arena, g_arena_bytes);
		g_arena = NULL;
	}
	g_free_list = NULL;
	g_inited = false;
	pthread_mutex_unlock(&g_lock);
}

void *host_mem_malloc(size_t size)
{
	void *p;

	if (!g_inited) {
		(void)host_mem_init();
	}
	if (!g_wanted || !g_arena) {
		return LIBC_MALLOC(size);
	}

	pthread_mutex_lock(&g_lock);
	/* Prefer internal DRAM for small blocks (system-heap-like); PSRAM for large. */
	p = alloc_from_free(size, size > 4096);
	pthread_mutex_unlock(&g_lock);
	return p;
}

void *host_mem_calloc(size_t nmemb, size_t size)
{
	size_t total;
	void *p;

	if (nmemb != 0 && size > SIZE_MAX / nmemb) {
		return NULL;
	}
	total = nmemb * size;
	p = host_mem_malloc(total);
	if (p) {
		memset(p, 0, total);
	}
	return p;
}

void host_mem_free(void *ptr)
{
	struct host_blk *b;

	if (!ptr) {
		return;
	}
	if (!g_inited) {
		(void)host_mem_init();
	}
	if (!g_wanted || !g_arena || !ptr_in_arena(ptr)) {
		LIBC_FREE(ptr);
		return;
	}

	pthread_mutex_lock(&g_lock);
	if (((uintptr_t)ptr % HOST_MEM_ALIGN) != 0) {
		g_stats.misalign_faults++;
		pthread_mutex_unlock(&g_lock);
		fault("free misaligned", ptr);
		return;
	}
	b = hdr_from_ptr(ptr);
	if (!ptr_in_arena(b)) {
		g_stats.oob_faults++;
		pthread_mutex_unlock(&g_lock);
		fault("free OOB header", ptr);
		return;
	}
	if (b->magic == BLK_MAGIC_FREE) {
		g_stats.double_free_faults++;
		pthread_mutex_unlock(&g_lock);
		fault("double free", ptr);
		return;
	}
	if (b->magic != BLK_MAGIC_USED) {
		g_stats.oob_faults++;
		pthread_mutex_unlock(&g_lock);
		fault("free bad magic", ptr);
		return;
	}
	if (!check_redzone(b)) {
		g_stats.canary_faults++;
		pthread_mutex_unlock(&g_lock);
		fault("redzone smashed (write past end?)", ptr);
		return;
	}

	if (g_stats.used_bytes >= b->capacity + sizeof(struct host_blk)) {
		g_stats.used_bytes -= b->capacity + sizeof(struct host_blk);
	}
	g_stats.frees++;
	g_stats.free_bytes = g_arena_bytes - g_stats.used_bytes;
	insert_free(b);
	coalesce();
	pthread_mutex_unlock(&g_lock);
}

void *host_mem_realloc(void *ptr, size_t size)
{
	void *n;
	struct host_blk *b;
	size_t old;

	if (!ptr) {
		return host_mem_malloc(size);
	}
	if (size == 0) {
		host_mem_free(ptr);
		return NULL;
	}
	if (!g_wanted || !g_arena || !ptr_in_arena(ptr)) {
		return LIBC_REALLOC(ptr, size);
	}

	pthread_mutex_lock(&g_lock);
	if (((uintptr_t)ptr % HOST_MEM_ALIGN) != 0) {
		g_stats.misalign_faults++;
		pthread_mutex_unlock(&g_lock);
		fault("realloc misaligned", ptr);
		return NULL;
	}
	b = hdr_from_ptr(ptr);
	if (b->magic != BLK_MAGIC_USED || !check_redzone(b)) {
		g_stats.canary_faults++;
		pthread_mutex_unlock(&g_lock);
		fault("realloc bad block", ptr);
		return NULL;
	}
	old = b->size;
	pthread_mutex_unlock(&g_lock);

	n = host_mem_malloc(size);
	if (!n) {
		return NULL;
	}
	memcpy(n, ptr, old < size ? old : size);
	host_mem_free(ptr);
	pthread_mutex_lock(&g_lock);
	g_stats.reallocs++;
	pthread_mutex_unlock(&g_lock);
	return n;
}

void *k_malloc(size_t size)
{
	return host_mem_malloc(size);
}

void k_free(void *ptr)
{
	host_mem_free(ptr);
}

void host_mem_get_stats(struct host_mem_stats *out)
{
	if (!out) {
		return;
	}
	pthread_mutex_lock(&g_lock);
	*out = g_stats;
	pthread_mutex_unlock(&g_lock);
}

void host_mem_log_stats(const char *tag)
{
	struct host_mem_stats s;

	host_mem_get_stats(&s);
	LOG_INF("%s arena=%zu used=%zu peak=%zu free=%zu allocs=%u frees=%u reallocs=%u fails=%u "
		"faults(mis=%u dbl=%u oob=%u canary=%u)",
		tag ? tag : "mem", s.arena_bytes, s.used_bytes, s.peak_used, s.free_bytes, s.allocs,
		s.frees, s.reallocs, s.fails, s.misalign_faults, s.double_free_faults, s.oob_faults,
		s.canary_faults);
}
