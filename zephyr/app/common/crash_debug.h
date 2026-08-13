#pragma once

#include <stdbool.h>

/** Non-production fault injection — compiled only with CONFIG_APP_CRASH_DEBUG. */
bool crash_debug_available(void);

/** Queue inject on main thread (returns immediately). kind: panic assert null badptr div0 unalign ill stack wdt */
bool crash_debug_schedule_inject(const char *kind);

/** Drain pending inject (main loop only). */
void crash_debug_poll(void);

/** Run inject synchronously (internal / testing). */
void crash_debug_inject(const char *kind);
