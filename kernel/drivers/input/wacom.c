#include "wacom.h"
#include "io.h"
#include "klog.h"

int wacom_init(void)
{
    klog_write("wacom: stub init\n");
    return 0;
}

void wacom_shutdown(void)
{
    klog_write("wacom: stub shutdown\n");
}
