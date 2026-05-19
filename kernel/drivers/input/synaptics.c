#include "synaptics.h"
#include "io.h"
#include "klog.h"

int synaptics_init(void)
{
    klog_write("synaptics: stub init\n");
    return 0;
}

void synaptics_shutdown(void)
{
    klog_write("synaptics: stub shutdown\n");
}
