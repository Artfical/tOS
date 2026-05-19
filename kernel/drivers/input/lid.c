#include "lid.h"
#include "io.h"
#include "klog.h"

int lid_init(void)
{
    klog_write("lid: stub init\n");
    return 0;
}

void lid_shutdown(void)
{
    klog_write("lid: stub shutdown\n");
}
