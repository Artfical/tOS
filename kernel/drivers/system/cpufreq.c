#include "cpufreq.h"
#include "io.h"
#include "klog.h"

int cpufreq_init(void)
{
    klog_write("cpufreq: stub init\n");
    return 0;
}

void cpufreq_shutdown(void)
{
    klog_write("cpufreq: stub shutdown\n");
}
