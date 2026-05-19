#include "uac.h"
#include "io.h"
#include "klog.h"

int uac_init(void)
{
    klog_write("uac: stub init\n");
    return 0;
}

void uac_shutdown(void)
{
    klog_write("uac: stub shutdown\n");
}
