#include "uvc.h"
#include "io.h"
#include "klog.h"

int uvc_init(void)
{
    klog_write("uvc: stub init\n");
    return 0;
}

void uvc_shutdown(void)
{
    klog_write("uvc: stub shutdown\n");
}
