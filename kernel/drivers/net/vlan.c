#include "vlan.h"
#include "io.h"
#include "klog.h"

int vlan_init(void)
{
    klog_write("vlan: stub init\n");
    return 0;
}

void vlan_shutdown(void)
{
    klog_write("vlan: stub shutdown\n");
}
