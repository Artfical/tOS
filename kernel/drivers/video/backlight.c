#include "backlight.h"
#include "io.h"
#include "klog.h"

int backlight_init(void)
{
    klog_write("backlight: stub init\n");
    return 0;
}

void backlight_shutdown(void)
{
    klog_write("backlight: stub shutdown\n");
}
