#pragma once

#include <stdint.h>

/** Scripted MT200 bridge stub (phone hold / start ignored) with log-calibrated delays. */
void mt200_host_init(void);
void mt200_host_tick(uint32_t now_ms);
uint32_t mt200_host_events(void);
