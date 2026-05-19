#include "zram.h"
#include "io.h"
#include "klog.h"

int zram_init(void)
{
    klog_write("zram: stub init\n");
    return 0;
}

void zram_shutdown(void)
{
    klog_write("zram: stub shutdown\n");
}
