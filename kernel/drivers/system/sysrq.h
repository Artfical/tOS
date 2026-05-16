#ifndef SYSRQ_H
#define SYSRQ_H
#include <stdint.h>
#define SYSRQ_KEY 0x54
typedef struct {
    int enabled;
    void (*reboot_handler)(void);
    void (*shutdown_handler)(void);
    void (*crash_handler)(void);
} sysrq_device_t;
void sysrq_init(sysrq_device_t *sysrq);
void sysrq_handle(char key);
#endif
