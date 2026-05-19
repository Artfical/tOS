#include "mem_hotplug.h"
#include "io.h"
#include "klog.h"

int mem_hotplug_init(void)
{
    klog_write("mem_hotplug: stub init\n");
    return 0;
}

void mem_hotplug_shutdown(void)
{
    klog_write("mem_hotplug: stub shutdown\n");
}
