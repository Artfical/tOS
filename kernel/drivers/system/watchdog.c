#include "watchdog.h"
#include "io.h"
int watchdog_init(watchdog_device_t *wdt)
{
    wdt->present = 0;
    wdt->io_base = 0;
    wdt->timeout_sec = 30;
    return -1;
}
void watchdog_pet(watchdog_device_t *wdt)
{
    (void)wdt;
}
