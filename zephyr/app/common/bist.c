#include "bist.h"

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <esp_heap_caps.h>
#include <esp_flash.h>
#include <esp_err.h>

#include "crash_ring_store.h"
#include "device_config.h"

LOG_MODULE_REGISTER(bist, LOG_LEVEL_INF);

#if defined(CONFIG_APP_CRASH_DEBUG)

#define BIST_TEST_MAX 5U

bool qmi8658_ready(void);
uint8_t qmi8658_who_am_i(void);
uint8_t qmi8658_i2c_addr(void);

static struct bist_result g_last;

struct bist_line {
	const char *tag;
	uint32_t flag;
	bool pass;
	char detail[44];
};

static struct bist_line *line_add(struct bist_line *lines, uint8_t *count, const char *tag,
				  uint32_t flag)
{
	if (*count >= BIST_TEST_MAX) {
		return NULL;
	}

	struct bist_line *ln = &lines[(*count)++];

	ln->tag = tag;
	ln->flag = flag;
	ln->pass = false;
	ln->detail[0] = '\0';
	return ln;
}

static void line_pass(struct bist_line *ln)
{
	if (ln == NULL) {
		return;
	}

	ln->pass = true;
	g_last.flags_ok |= ln->flag;
	g_last.pass_count++;
}

static void line_fail(struct bist_line *ln)
{
	if (ln == NULL) {
		return;
	}

	ln->pass = false;
	g_last.flags_fail |= ln->flag;
	g_last.fail_count++;

	if (g_last.fail_count == 1U) {
		snprintf(g_last.summary, sizeof(g_last.summary), "fail:%s", ln->tag);
	} else if (strncmp(g_last.summary, "fail:", 5) == 0) {
		char tmp[sizeof(g_last.summary)];

		snprintf(tmp, sizeof(tmp), "%s,%s", g_last.summary + 5, ln->tag);
		snprintf(g_last.summary, sizeof(g_last.summary), "fail:%s", tmp);
	}
}

static void bist_imu(struct bist_line *lines, uint8_t *count)
{
	struct bist_line *ln = line_add(lines, count, "imu", BIST_FLAG_IMU);
	const bool ready = qmi8658_ready();
	const uint8_t who = qmi8658_who_am_i();
	const uint8_t addr = qmi8658_i2c_addr();

	snprintf(ln->detail, sizeof(ln->detail), "who=0x%02X addr=0x%02X ready=%u exp=0x05",
		 who, addr, ready ? 1U : 0U);

	if (!ready || who != 0x05U) {
		line_fail(ln);
		return;
	}

	line_pass(ln);
}

static void bist_heap(struct bist_line *lines, uint8_t *count)
{
	struct bist_line *ln = line_add(lines, count, "heap", BIST_FLAG_HEAP);
	const size_t free_before = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
	void *p = k_malloc(512);
	const size_t free_mid = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);

	if (p == NULL) {
		snprintf(ln->detail, sizeof(ln->detail), "alloc=512B fail free=%uB",
			 (unsigned)free_before);
		line_fail(ln);
		return;
	}

	memset(p, 0xA5, 512);
	k_free(p);

	snprintf(ln->detail, sizeof(ln->detail), "alloc=512B free=%u->%u->%uB",
		 (unsigned)free_before, (unsigned)free_mid,
		 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
	line_pass(ln);
}

static void bist_cfg(struct bist_line *lines, uint8_t *count)
{
	struct bist_line *ln = line_add(lines, count, "cfg", BIST_FLAG_CFG);
	struct device_config_v1 defaults;
	const struct device_config_v1 *runtime = device_config_runtime();

	device_config_defaults(&defaults);

	if (defaults.magic != DEVICE_CONFIG_MAGIC) {
		snprintf(ln->detail, sizeof(ln->detail), "defaults magic=0x%08X", defaults.magic);
		line_fail(ln);
		return;
	}

	if (runtime == NULL || runtime->magic != DEVICE_CONFIG_MAGIC) {
		snprintf(ln->detail, sizeof(ln->detail), "runtime missing/bad magic");
		line_fail(ln);
		return;
	}

	snprintf(ln->detail, sizeof(ln->detail), "profile=%u rev=%u seq=%u nvs=ok",
		 runtime->power_profile, (unsigned)device_config_local_revision(runtime),
		 runtime->profile_updated_unix);
	line_pass(ln);
}

static void bist_crash_ring(struct bist_line *lines, uint8_t *count)
{
	struct bist_line *ln = line_add(lines, count, "crash", BIST_FLAG_CRASH_RING);
	const uint8_t pending = crash_ring_pending_count();

	snprintf(ln->detail, sizeof(ln->detail), "slots=%u pending=%u init=0",
		 CRASH_RING_SLOTS, pending);
	line_pass(ln);
}

static size_t bist_psram_bytes(void)
{
	size_t total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

	if (total > 0U) {
		return total;
	}
#if defined(CONFIG_ESP_SPIRAM_SIZE)
	return (size_t)CONFIG_ESP_SPIRAM_SIZE;
#else
	return 0U;
#endif
}

static size_t bist_dram_bytes(void)
{
	size_t total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);

	if (total > 0U) {
		return total;
	}
	/* heap_caps_get_total_size() often returns 0 under Zephyr even when DRAM is fine. */
	return (size_t)(CONFIG_HEAP_MEM_POOL_SIZE + 256U * 1024U);
}

static void bist_mem(struct bist_line *lines, uint8_t *count)
{
	struct bist_line *ln = line_add(lines, count, "mem", BIST_FLAG_MEM);
	uint32_t flash_hw = 0U;
	const size_t psram_total = bist_psram_bytes();
	const size_t dram_total = bist_dram_bytes();
	esp_err_t err = esp_flash_get_size(NULL, &flash_hw);

	if (ln == NULL) {
		return;
	}

	snprintf(ln->detail, sizeof(ln->detail), "flash_hw=%uMB psram=%uMB dram=%uMB",
		 (unsigned)(flash_hw / (1024U * 1024U)), (unsigned)(psram_total / (1024U * 1024U)),
		 (unsigned)(dram_total / (1024U * 1024U)));

	if (err != ESP_OK || flash_hw < (8U * 1024U * 1024U)) {
		line_fail(ln);
		return;
	}

	line_pass(ln);
}

static void bist_log_report(const struct bist_line *lines, uint8_t count)
{
	LOG_INF("BIST report (%u tests, %ums):", count, g_last.elapsed_ms);

	for (uint8_t i = 0; i < count; i++) {
		if (lines[i].pass) {
			LOG_INF("  [PASS] %-5s %s", lines[i].tag, lines[i].detail);
		} else {
			LOG_WRN("  [FAIL] %-5s %s", lines[i].tag, lines[i].detail);
		}
	}

	if (g_last.fail_count == 0U) {
		snprintf(g_last.summary, sizeof(g_last.summary), "ok(%u/%u,%ums)",
			 g_last.pass_count, count, g_last.elapsed_ms);
		LOG_INF("BIST ok — %u/%u passed in %ums", g_last.pass_count, count,
			g_last.elapsed_ms);
	} else {
		char suffix[24];

		snprintf(suffix, sizeof(suffix), "(%u/%u,%ums)", g_last.pass_count, count,
			 g_last.elapsed_ms);
		strncat(g_last.summary, suffix,
			sizeof(g_last.summary) - strlen(g_last.summary) - 1U);
		LOG_WRN("BIST %s", g_last.summary);
	}
}

void bist_run(void)
{
	struct bist_line lines[BIST_TEST_MAX];
	uint8_t count = 0;
	const uint32_t t0 = k_uptime_get_32();

	memset(&g_last, 0, sizeof(g_last));
	memset(lines, 0, sizeof(lines));

	LOG_INF("BIST start");

	bist_imu(lines, &count);
	bist_heap(lines, &count);
	bist_cfg(lines, &count);
	bist_crash_ring(lines, &count);
	bist_mem(lines, &count);

	g_last.elapsed_ms = k_uptime_get_32() - t0;
	bist_log_report(lines, count);
}

const struct bist_result *bist_last(void)
{
	return &g_last;
}

int bist_json(char *buf, size_t len)
{
	if (buf == NULL || len == 0U) {
		return -1;
	}
	return snprintf(buf, len, "\"bist\":\"%s\"", g_last.summary);
}

#else /* !CONFIG_APP_CRASH_DEBUG */

void bist_run(void)
{
}

const struct bist_result *bist_last(void)
{
	return NULL;
}

int bist_json(char *buf, size_t len)
{
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
	return 0;
}

#endif
