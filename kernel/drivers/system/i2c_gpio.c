#include "i2c_gpio.h"
#include "io.h"
#include "klog.h"

int i2c_gpio_init(void)
{
    klog_write("i2c_gpio: stub init\n");
    return 0;
}

void i2c_gpio_shutdown(void)
{
    klog_write("i2c_gpio: stub shutdown\n");
}
