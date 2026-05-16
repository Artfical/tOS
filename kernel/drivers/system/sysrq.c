#include "sysrq.h"
#include <stddef.h>
static sysrq_device_t global_sysrq;
void sysrq_init(sysrq_device_t *sysrq)
{
    sysrq->enabled = 1;
    sysrq->reboot_handler = NULL;
    sysrq->shutdown_handler = NULL;
    sysrq->crash_handler = NULL;
    global_sysrq = *sysrq;
}
void sysrq_handle(char key)
{
    if (!global_sysrq.enabled) return;
    switch (key) {
        case 'r':
            if (global_sysrq.reboot_handler) global_sysrq.reboot_handler();
            break;
        case 's':
            if (global_sysrq.shutdown_handler) global_sysrq.shutdown_handler();
            break;
        case 'c':
            if (global_sysrq.crash_handler) global_sysrq.crash_handler();
            break;
    }
}
