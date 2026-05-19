#include "gus.h"
#include "io.h"
#include "klog.h"

int gus_init(void)
{
    klog_write("gus: stub init\n");
    return 0;
}

void gus_shutdown(void)
{
    klog_write("gus: stub shutdown\n");
}
