#include "power_btn.h"
#include "io.h"
#include "klog.h"

int power_btn_init(void)
{
    klog_write("power_btn: stub init\n");
    return 0;
}

void power_btn_shutdown(void)
{
    klog_write("power_btn: stub shutdown\n");
}
