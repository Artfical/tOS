#include "led.h"
#include "io.h"
#include "klog.h"

int led_init(void)
{
    klog_write("led: stub init\n");
    return 0;
}

void led_shutdown(void)
{
    klog_write("led: stub shutdown\n");
}
