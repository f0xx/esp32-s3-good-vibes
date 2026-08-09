#pragma once

#include <stdbool.h>

void boot_button_init(void);
void boot_button_poll(void);
bool boot_button_take_toggle_request(void);
bool boot_button_screen_on(void);
