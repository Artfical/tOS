#include "bridge.h"
#include "io.h"
#include "klog.h"

int bridge_init(void)
{
    klog_write("bridge: stub init\n");
    return 0;
}

void bridge_shutdown(void)
{
    klog_write("bridge: stub shutdown\n");
}
