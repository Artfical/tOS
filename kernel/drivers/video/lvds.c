#include "lvds.h"
#include "io.h"
#include "klog.h"

int lvds_init(void)
{
    klog_write("lvds: stub init\n");
    return 0;
}

void lvds_shutdown(void)
{
    klog_write("lvds: stub shutdown\n");
}
