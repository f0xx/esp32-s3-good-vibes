#pragma once

#include <stdbool.h>
#include <stdint.h>

int battery_adc_init(void);
bool battery_adc_cal_ok(void);
int battery_adc_read_raw(int *raw_out);
int battery_adc_read_mv(uint16_t *mv_out);
int battery_adc_debug_atten(void);
