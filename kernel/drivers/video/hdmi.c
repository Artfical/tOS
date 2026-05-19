#include "hdmi.h"
#include "io.h"
#include "klog.h"

int hdmi_init(void)
{
    klog_write("hdmi: stub init\n");
    return 0;
}

void hdmi_shutdown(void)
{
    klog_write("hdmi: stub shutdown\n");
}
