#ifndef WATCHDOG_H
#define WATCHDOG_H
#include <stdint.h>
#define WDT_IO_BASE 0x60
typedef struct {
    int present;
    uint16_t io_base;
    int timeout_sec;
} watchdog_device_t;
int watchdog_init(watchdog_device_t *wdt);
void watchdog_pet(watchdog_device_t *wdt);
#endif
