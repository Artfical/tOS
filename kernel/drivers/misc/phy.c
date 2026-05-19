#include "phy.h"
#include "io.h"
#include "klog.h"

int phy_init(void)
{
    klog_write("phy: stub init\n");
    return 0;
}

void phy_shutdown(void)
{
    klog_write("phy: stub shutdown\n");
}
