#include "netfilter.h"
#include "io.h"
#include "klog.h"

int netfilter_init(void)
{
    klog_write("netfilter: stub init\n");
    return 0;
}

void netfilter_shutdown(void)
{
    klog_write("netfilter: stub shutdown\n");
}
