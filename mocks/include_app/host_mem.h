#pragma once

/*
 * Host ESP32-S3-ish flat memory model.
 *
 * One contiguous arena is mapped at init:
 *   [0 .. DRAM_BYTES)     internal SRAM stand-in (512 KiB)
 *   [DRAM_BYTES .. end)   PSRAM stand-in (8 MiB, board CONFIG_ESP_SPIRAM_SIZE)
 *
 * malloc/realloc/free (via --wrap, when enabled) and k_malloc/k_free carve
 * blocks from this arena so OOB / misalignment / double-free show up against
 * a fixed address space instead of glibc's heap.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** ESP32-S3-ish sizes (desk Waveshare 8 MB octal PSRAM). */
enum {
	HOST_MEM_DRAM_BYTES = 512 * 1024,
	HOST_MEM_PSRAM_BYTES = 8 * 1024 * 1024,
	HOST_MEM_TOTAL_BYTES = HOST_MEM_DRAM_BYTES + HOST_MEM_PSRAM_BYTES,
	/** Xtensa / ESP-IDF style minimum payload alignment. */
	HOST_MEM_ALIGN = 8,
};

struct host_mem_stats {
	size_t arena_bytes;
	size_t dram_bytes;
	size_t psram_bytes;
	size_t used_bytes;
	size_t free_bytes;
	size_t peak_used;
	uint32_t allocs;
	uint32_t frees;
	uint32_t reallocs;
	uint32_t fails;
	uint32_t misalign_faults;
	uint32_t double_free_faults;
	uint32_t oob_faults;
	uint32_t canary_faults;
};

/** Map arena. Safe to call more than once. Returns 0 on success. */
int host_mem_init(void);
void host_mem_shutdown(void);
bool host_mem_enabled(void);
void host_mem_set_wanted(bool want);

void *host_mem_malloc(size_t size);
void *host_mem_calloc(size_t nmemb, size_t size);
void *host_mem_realloc(void *ptr, size_t size);
void host_mem_free(void *ptr);

/** Zephyr-style aliases used by firmware units on host. */
void *k_malloc(size_t size);
void k_free(void *ptr);

void host_mem_get_stats(struct host_mem_stats *out);
void host_mem_log_stats(const char *tag);

/** True if ptr lies inside the mapped arena (any region). */
bool host_mem_contains(const void *ptr);

#ifdef __cplusplus
}
#endif
