#include "apm.h"
int apm_init(apm_device_t *dev)
{
    dev->present = 0;
    dev->version_major = 0;
    dev->version_minor = 0;
    dev->cpus = 0;
    return -1;
}
int apm_set_state(apm_device_t *dev, int state)
{
    (void)dev;
    (void)state;
    return -1;
}
