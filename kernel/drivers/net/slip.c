#include "slip.h"
#include "io.h"
#include "klog.h"

int slip_init(void)
{
    klog_write("slip: stub init\n");
    return 0;
}

void slip_shutdown(void)
{
    klog_write("slip: stub shutdown\n");
}
