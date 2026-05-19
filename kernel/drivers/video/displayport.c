#include "displayport.h"
#include "io.h"
#include "klog.h"

int displayport_init(void)
{
    klog_write("displayport: stub init\n");
    return 0;
}

void displayport_shutdown(void)
{
    klog_write("displayport: stub shutdown\n");
}
