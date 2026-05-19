#include "i2c_rtc.h"
#include "io.h"
#include "klog.h"

int i2c_rtc_init(void)
{
    klog_write("i2c_rtc: stub init\n");
    return 0;
}

void i2c_rtc_shutdown(void)
{
    klog_write("i2c_rtc: stub shutdown\n");
}
