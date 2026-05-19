#include "touchscreen.h"
#include "io.h"
#include "klog.h"

int touchscreen_init(void)
{
    klog_write("touchscreen: stub init\n");
    return 0;
}

void touchscreen_shutdown(void)
{
    klog_write("touchscreen: stub shutdown\n");
}
