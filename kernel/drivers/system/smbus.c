#include "smbus.h"
#include "io.h"
#include "klog.h"

int smbus_init(void)
{
    klog_write("smbus: stub init\n");
    return 0;
}

void smbus_shutdown(void)
{
    klog_write("smbus: stub shutdown\n");
}
