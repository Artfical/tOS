#include "wireless.h"
#include "io.h"
#include "klog.h"

int wireless_init(void)
{
    klog_write("wireless: stub init\n");
    return 0;
}

void wireless_shutdown(void)
{
    klog_write("wireless: stub shutdown\n");
}
