#pragma once

#include <stdbool.h>

int chip_temp_init(void);
void chip_temp_tick(void);
float chip_temp_celsius(void);
bool chip_temp_valid(void);
