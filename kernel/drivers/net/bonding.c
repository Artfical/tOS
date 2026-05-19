#include "bonding.h"
#include "io.h"
#include "klog.h"

int bonding_init(void)
{
    klog_write("bonding: stub init\n");
    return 0;
}

void bonding_shutdown(void)
{
    klog_write("bonding: stub shutdown\n");
}
