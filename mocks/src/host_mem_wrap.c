/*
 * Optional libc malloc wrappers → host flat arena.
 * Linked only when HOST_FLAT_HEAP_WRAP=1 (plain / TSan builds; not ASan).
 */

#include "host_mem.h"

#include <stddef.h>

void *__real_malloc(size_t size);
void *__real_calloc(size_t nmemb, size_t size);
void *__real_realloc(void *ptr, size_t size);
void __real_free(void *ptr);

void *__wrap_malloc(size_t size)
{
	(void)host_mem_init();
	if (!host_mem_enabled()) {
		return __real_malloc(size);
	}
	return host_mem_malloc(size);
}

void *__wrap_calloc(size_t nmemb, size_t size)
{
	(void)host_mem_init();
	if (!host_mem_enabled()) {
		return __real_calloc(nmemb, size);
	}
	return host_mem_calloc(nmemb, size);
}

void *__wrap_realloc(void *ptr, size_t size)
{
	(void)host_mem_init();
	if (!host_mem_enabled()) {
		return __real_realloc(ptr, size);
	}
	return host_mem_realloc(ptr, size);
}

void __wrap_free(void *ptr)
{
	if (!ptr) {
		return;
	}
	(void)host_mem_init();
	if (!host_mem_enabled() || !host_mem_contains(ptr)) {
		__real_free(ptr);
		return;
	}
	host_mem_free(ptr);
}
