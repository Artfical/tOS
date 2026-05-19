#include "raid.h"
#include "io.h"
#include "klog.h"

int raid_init(void)
{
    klog_write("raid: stub init\n");
    return 0;
}

void raid_shutdown(void)
{
    klog_write("raid: stub shutdown\n");
}
