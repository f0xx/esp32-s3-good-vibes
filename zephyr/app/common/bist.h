#pragma once

#include <stddef.h>
#include <stdint.h>

#define BIST_FLAG_IMU        (1U << 0)
#define BIST_FLAG_HEAP       (1U << 1)
#define BIST_FLAG_CFG        (1U << 2)
#define BIST_FLAG_CRASH_RING (1U << 3)

struct bist_result {
	uint32_t flags_ok;
	uint32_t flags_fail;
	char summary[32];
	uint32_t elapsed_ms;
	uint8_t pass_count;
	uint8_t fail_count;
};

/** Run all self-tests (boot or BLE on-demand). */
void bist_run(void);

const struct bist_result *bist_last(void);

/** JSON fragment: "bist":"ok(4/4,12ms)" or "bist":"fail:imu(2/4,15ms)" */
int bist_json(char *buf, size_t len);
