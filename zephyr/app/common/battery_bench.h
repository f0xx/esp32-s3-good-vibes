#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BATTERY_BENCH_SAMPLE_MS 1000U

struct battery_bench_snapshot {
	uint32_t session_id;
	uint32_t sample_seq;
	uint32_t uptime_ms;
	uint8_t cpu_mhz;
	uint8_t imu_hz;
	uint8_t render_hz;
	uint8_t screen_on;
};

int battery_bench_init(void);
bool battery_bench_active(void);
bool battery_bench_config_locked(void);
uint32_t battery_bench_session_id(void);
uint32_t battery_bench_sample_seq(void);
int battery_bench_start(void);
int battery_bench_stop(void);
/** Returns true when a new bench sample was taken (caller should push STATUS). */
bool battery_bench_tick(uint32_t now_ms);
/** Auto-stop on low cell / DC; abort unsafe NVS resume after brownout. Call after battery_monitor_tick().
 *  Returns true if bench was auto-stopped this poll (caller may push STATUS). */
bool battery_bench_safety_poll(void);
void battery_bench_snapshot_fill(struct battery_bench_snapshot *out);
