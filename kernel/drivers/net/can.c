#include "can.h"
#include "io.h"
#include "klog.h"

int can_init(void)
{
    klog_write("can: stub init\n");
    return 0;
}

void can_shutdown(void)
{
    klog_write("can: stub shutdown\n");
}
