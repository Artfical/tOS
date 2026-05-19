#include "rng_core.h"
#include "io.h"
#include "klog.h"

int rng_core_init(void)
{
    klog_write("rng_core: stub init\n");
    return 0;
}

void rng_core_shutdown(void)
{
    klog_write("rng_core: stub shutdown\n");
}
