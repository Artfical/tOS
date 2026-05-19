#include "lvm.h"
#include "io.h"
#include "klog.h"

int lvm_init(void)
{
    klog_write("lvm: stub init\n");
    return 0;
}

void lvm_shutdown(void)
{
    klog_write("lvm: stub shutdown\n");
}
