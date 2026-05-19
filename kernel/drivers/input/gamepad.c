#include "gamepad.h"
#include "io.h"
#include "klog.h"

int gamepad_init(void)
{
    klog_write("gamepad: stub init\n");
    return 0;
}

void gamepad_shutdown(void)
{
    klog_write("gamepad: stub shutdown\n");
}
