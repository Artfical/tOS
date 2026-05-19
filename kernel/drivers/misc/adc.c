#include "adc.h"
#include "io.h"
#include "klog.h"

int adc_init(void)
{
    klog_write("adc: stub init\n");
    return 0;
}

void adc_shutdown(void)
{
    klog_write("adc: stub shutdown\n");
}
