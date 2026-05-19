#include "pwm_core.h"
#include "io.h"
#include "klog.h"

int pwm_core_init(void)
{
    klog_write("pwm_core: stub init\n");
    return 0;
}

void pwm_core_shutdown(void)
{
    klog_write("pwm_core: stub shutdown\n");
}
