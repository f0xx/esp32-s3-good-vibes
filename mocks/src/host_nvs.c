#include "host_nvs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/logging/log.h>

#include "host_debug.h"

LOG_MODULE_REGISTER(host_nvs, LOG_LEVEL_INF);

enum { HOST_NVS_MAX_ENTRIES = 64, HOST_NVS_KEY_MAX = 48, HOST_NVS_VAL_MAX = 512 };

struct nvs_entry {
	char key[HOST_NVS_KEY_MAX];
	uint16_t len;
	uint8_t data[HOST_NVS_VAL_MAX];
	bool used;
};

static struct nvs_entry g_entries[HOST_NVS_MAX_ENTRIES];
static char g_path[512] = "mocks_nvs.bin";
static bool g_loaded;

static int find_key(const char *key)
{
	for (int i = 0; i < HOST_NVS_MAX_ENTRIES; i++) {
		if (g_entries[i].used && strcmp(g_entries[i].key, key) == 0) {
			return i;
		}
	}
	return -1;
}

static int find_free(void)
{
	for (int i = 0; i < HOST_NVS_MAX_ENTRIES; i++) {
		if (!g_entries[i].used) {
			return i;
		}
	}
	return -1;
}

static int persist(void)
{
	FILE *f = fopen(g_path, "wb");

	if (!f) {
		LOG_ERR("nvs persist fopen(%s) errno=%d", g_path, errno);
		return -1;
	}
	fwrite(g_entries, sizeof(g_entries), 1, f);
	fclose(f);
	HOST_LOG(HOST_LOG_DBG, "host_nvs", "persisted %s", g_path);
	return 0;
}

static void load(void)
{
	FILE *f;

	if (g_loaded) {
		return;
	}
	g_loaded = true;
	memset(g_entries, 0, sizeof(g_entries));
	f = fopen(g_path, "rb");
	if (!f) {
		LOG_INF("nvs empty/new path=%s", g_path);
		return;
	}
	if (fread(g_entries, sizeof(g_entries), 1, f) != 1) {
		LOG_WRN("nvs load truncated — reset");
		memset(g_entries, 0, sizeof(g_entries));
	}
	fclose(f);
	LOG_INF("nvs loaded path=%s", g_path);
}

void host_nvs_set_path(const char *path)
{
	if (path && path[0]) {
		snprintf(g_path, sizeof(g_path), "%s", path);
	}
	g_loaded = false;
}

const char *host_nvs_path(void)
{
	return g_path;
}

void host_nvs_init(const char *path)
{
	host_nvs_set_path(path ? path : "mocks_nvs.bin");
	load();
}

int host_nvs_write(const char *key, const void *data, size_t len)
{
	int idx;

	if (!key || !data || len > HOST_NVS_VAL_MAX || strlen(key) >= HOST_NVS_KEY_MAX) {
		return -EINVAL;
	}
	load();
	idx = find_key(key);
	if (idx < 0) {
		idx = find_free();
	}
	if (idx < 0) {
		LOG_ERR("nvs full");
		return -ENOSPC;
	}
	memset(&g_entries[idx], 0, sizeof(g_entries[idx]));
	snprintf(g_entries[idx].key, sizeof(g_entries[idx].key), "%s", key);
	memcpy(g_entries[idx].data, data, len);
	g_entries[idx].len = (uint16_t)len;
	g_entries[idx].used = true;
	LOG_DBG("nvs write key=%s len=%zu", key, len);
	return persist();
}

int host_nvs_read(const char *key, void *out, size_t cap, size_t *out_len)
{
	int idx;

	if (!key || !out) {
		return -EINVAL;
	}
	load();
	idx = find_key(key);
	if (idx < 0) {
		return -ENOENT;
	}
	if (cap < g_entries[idx].len) {
		return -ENOMEM;
	}
	memcpy(out, g_entries[idx].data, g_entries[idx].len);
	if (out_len) {
		*out_len = g_entries[idx].len;
	}
	return 0;
}

int host_nvs_delete(const char *key)
{
	int idx;

	if (!key) {
		return -EINVAL;
	}
	load();
	idx = find_key(key);
	if (idx < 0) {
		return -ENOENT;
	}
	g_entries[idx].used = false;
	return persist();
}

void host_nvs_clear(void)
{
	memset(g_entries, 0, sizeof(g_entries));
	g_loaded = true;
	(void)persist();
	LOG_WRN("nvs cleared");
}

int host_crash_ring_append(const void *data, size_t len)
{
	char key[HOST_NVS_KEY_MAX];
	uint32_t count = 0;
	size_t got = 0;

	if (host_nvs_read("crash.count", &count, sizeof(count), &got) != 0) {
		count = 0;
	}
	snprintf(key, sizeof(key), "crash.%u", count);
	if (host_nvs_write(key, data, len) != 0) {
		return -1;
	}
	count++;
	return host_nvs_write("crash.count", &count, sizeof(count));
}

size_t host_crash_ring_count(void)
{
	uint32_t count = 0;
	size_t got = 0;

	if (host_nvs_read("crash.count", &count, sizeof(count), &got) != 0) {
		return 0;
	}
	return count;
}

int host_crash_ring_get(size_t index, void *out, size_t cap, size_t *out_len)
{
	char key[HOST_NVS_KEY_MAX];

	snprintf(key, sizeof(key), "crash.%zu", index);
	return host_nvs_read(key, out, cap, out_len);
}
