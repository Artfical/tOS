#ifndef APM_H
#define APM_H
#include <stdint.h>
#define APM_CMD_INSTALLATION_CHECK 0x5300
#define APM_CMD_REAL_MODE_CONNECT 0x5301
#define APM_CMD_PROT_MODE_CONNECT 0x5302
#define APM_CMD_DRIVER_VERSION 0x530E
#define APM_CMD_CPU_IDLE 0x5305
#define APM_CMD_CPU_BUSY 0x5306
#define APM_CMD_SET_STATE 0x5307
#define APM_STATE_STANDBY 1
#define APM_STATE_SUSPEND 2
#define APM_STATE_OFF 3
typedef struct {
    int present;
    int version_major;
    int version_minor;
    int cpus;
} apm_device_t;
int apm_init(apm_device_t *dev);
int apm_set_state(apm_device_t *dev, int state);
#endif
