#include "simplefb.h"
#include "io.h"
#include "klog.h"

int simplefb_init(void)
{
    klog_write("simplefb: stub init\n");
    return 0;
}

void simplefb_shutdown(void)
{
    klog_write("simplefb: stub shutdown\n");
}
