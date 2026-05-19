#include "rtc.h"
#include "io.h"
#include "klog.h"

int rtc_init(void)
{
    klog_write("rtc: stub init\n");
    return 0;
}

void rtc_shutdown(void)
{
    klog_write("rtc: stub shutdown\n");
}
