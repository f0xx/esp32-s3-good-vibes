#pragma once

#include <stdbool.h>

int ble_crash_gatt_init(void);
bool ble_crash_gatt_pending(void);
void ble_crash_gatt_looper_tick(void);
