#include "gpio_core.h"
#include "io.h"
#include "klog.h"

int gpio_core_init(void)
{
    klog_write("gpio_core: stub init\n");
    return 0;
}

void gpio_core_shutdown(void)
{
    klog_write("gpio_core: stub shutdown\n");
}
