#include "iscsi.h"
#include "io.h"
#include "klog.h"

int iscsi_init(void)
{
    klog_write("iscsi: stub init\n");
    return 0;
}

void iscsi_shutdown(void)
{
    klog_write("iscsi: stub shutdown\n");
}
