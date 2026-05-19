#include "hwmon_volt.h"
#include "io.h"
#include "klog.h"

int hwmon_volt_init(void)
{
    klog_write("hwmon_volt: stub init\n");
    return 0;
}

void hwmon_volt_shutdown(void)
{
    klog_write("hwmon_volt: stub shutdown\n");
}
