#include "mmc_block.h"
#include "io.h"
#include "klog.h"

int mmc_block_init(void)
{
    klog_write("mmc_block: stub init\n");
    return 0;
}

void mmc_block_shutdown(void)
{
    klog_write("mmc_block: stub shutdown\n");
}
