#include "dac.h"
#include "io.h"
#include "klog.h"

int dac_init(void)
{
    klog_write("dac: stub init\n");
    return 0;
}

void dac_shutdown(void)
{
    klog_write("dac: stub shutdown\n");
}
