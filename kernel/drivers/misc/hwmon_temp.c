#include "hwmon_temp.h"
#include "io.h"
#include "klog.h"

int hwmon_temp_init(void)
{
    klog_write("hwmon_temp: stub init\n");
    return 0;
}

void hwmon_temp_shutdown(void)
{
    klog_write("hwmon_temp: stub shutdown\n");
}
