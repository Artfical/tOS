#include "remote.h"
#include "io.h"
#include "klog.h"

int remote_init(void)
{
    klog_write("remote: stub init\n");
    return 0;
}

void remote_shutdown(void)
{
    klog_write("remote: stub shutdown\n");
}
