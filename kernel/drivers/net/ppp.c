#include "ppp.h"
#include "io.h"
#include "klog.h"

int ppp_init(void)
{
    klog_write("ppp: stub init\n");
    return 0;
}

void ppp_shutdown(void)
{
    klog_write("ppp: stub shutdown\n");
}
