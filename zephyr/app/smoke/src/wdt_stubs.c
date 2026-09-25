/* Smoke does not arm the soft stall WDT — satisfy renderer references. */

void stall_watchdog_feed_render(void)
{
}

void stall_watchdog_feed_main(void)
{
}
