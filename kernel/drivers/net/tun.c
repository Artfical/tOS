#include "tun.h"
#include "io.h"
#include "klog.h"

int tun_init(void)
{
    klog_write("tun: stub init\n");
    return 0;
}

void tun_shutdown(void)
{
    klog_write("tun: stub shutdown\n");
}
