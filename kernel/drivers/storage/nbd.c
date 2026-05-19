#include "nbd.h"
#include "io.h"
#include "klog.h"

int nbd_init(void)
{
    klog_write("nbd: stub init\n");
    return 0;
}

void nbd_shutdown(void)
{
    klog_write("nbd: stub shutdown\n");
}
