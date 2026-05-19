#include "hwmon_fan.h"
#include "io.h"
#include "klog.h"

int hwmon_fan_init(void)
{
    klog_write("hwmon_fan: stub init\n");
    return 0;
}

void hwmon_fan_shutdown(void)
{
    klog_write("hwmon_fan: stub shutdown\n");
}
