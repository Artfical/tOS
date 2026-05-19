#include "edac.h"
#include "io.h"
#include "klog.h"

int edac_init(void)
{
    klog_write("edac: stub init\n");
    return 0;
}

void edac_shutdown(void)
{
    klog_write("edac: stub shutdown\n");
}
