#include "cpuidle.h"
#include "io.h"
#include "klog.h"

int cpuidle_init(void)
{
    klog_write("cpuidle: stub init\n");
    return 0;
}

void cpuidle_shutdown(void)
{
    klog_write("cpuidle: stub shutdown\n");
}
