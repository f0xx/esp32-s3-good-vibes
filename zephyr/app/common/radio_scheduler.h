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

/**
 * Take/release exclusive panel-SPI hold for a radio epoch (MT200 session).
 * Caller must wait for renderer_busy()==false before hold=true.
 */
void radio_scheduler_panel_hold(bool hold);

/** True when panel SPI may run (no wifi/prep/MT200 panel hold). */
bool radio_scheduler_panel_spi_allowed(void);
