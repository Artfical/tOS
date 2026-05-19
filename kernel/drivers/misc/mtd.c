#include "mtd.h"
#include "io.h"
#include "klog.h"

int mtd_init(void)
{
    klog_write("mtd: stub init\n");
    return 0;
}

void mtd_shutdown(void)
{
    klog_write("mtd: stub shutdown\n");
}
