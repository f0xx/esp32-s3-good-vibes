#pragma once

#include <stdbool.h>

#include "vibro_schedule.h"

typedef void (*radio_ble_pause_fn)(bool paused);

void radio_scheduler_init(radio_ble_pause_fn pause_fn);
void radio_scheduler_sync(void);
void radio_scheduler_set_wifi_busy(bool busy);
void radio_scheduler_set_capture_prep(bool prep);
bool radio_scheduler_wifi_busy(void);
bool radio_scheduler_capture_prep(void);
const char *radio_scheduler_mode_str(void);
