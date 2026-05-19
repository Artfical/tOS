#include "ioat.h"
#include "io.h"
#include "klog.h"

int ioat_init(void)
{
    klog_write("ioat: stub init\n");
    return 0;
}

void ioat_shutdown(void)
{
    klog_write("ioat: stub shutdown\n");
}
