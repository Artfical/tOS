#include "pppoe.h"
#include "io.h"
#include "klog.h"

int pppoe_init(void)
{
    klog_write("pppoe: stub init\n");
    return 0;
}

void pppoe_shutdown(void)
{
    klog_write("pppoe: stub shutdown\n");
}
