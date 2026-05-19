#include "nvram.h"
#include "io.h"
#include "klog.h"

int nvram_init(void)
{
    klog_write("nvram: stub init\n");
    return 0;
}

void nvram_shutdown(void)
{
    klog_write("nvram: stub shutdown\n");
}
