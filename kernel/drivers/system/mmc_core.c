#include "mmc_core.h"
#include "io.h"
#include "klog.h"

int mmc_core_init(void)
{
    klog_write("mmc_core: stub init\n");
    return 0;
}

void mmc_core_shutdown(void)
{
    klog_write("mmc_core: stub shutdown\n");
}
