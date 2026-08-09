#pragma once

#include <stdbool.h>
#include <stdint.h>

struct battery_state {
	float voltage_v;
	uint8_t percent;
	float trend_v;
	bool on_dc;
	bool valid;
	uint16_t adc_mv;
};

int battery_monitor_init(void);
void battery_monitor_tick(void);
void battery_monitor_settle(uint32_t ms);
const struct battery_state *battery_monitor_state(void);
