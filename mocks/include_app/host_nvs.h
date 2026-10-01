#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Tiny file-backed key/value blob store (host stand-in for NVS / crash ring). */
void host_nvs_init(const char *path);
void host_nvs_set_path(const char *path);
const char *host_nvs_path(void);

int host_nvs_write(const char *key, const void *data, size_t len);
int host_nvs_read(const char *key, void *out, size_t cap, size_t *out_len);
int host_nvs_delete(const char *key);
void host_nvs_clear(void);

/** Crash-ring style append-only slots in the same file namespace. */
int host_crash_ring_append(const void *data, size_t len);
size_t host_crash_ring_count(void);
int host_crash_ring_get(size_t index, void *out, size_t cap, size_t *out_len);
