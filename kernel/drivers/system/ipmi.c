#include "ipmi.h"
#include "io.h"
#include "klog.h"

int ipmi_init(void)
{
    klog_write("ipmi: stub init\n");
    return 0;
}

void ipmi_shutdown(void)
{
    klog_write("ipmi: stub shutdown\n");
}
