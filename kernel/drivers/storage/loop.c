#include "loop.h"
#include "io.h"
#include "klog.h"

int loop_init(void)
{
    klog_write("loop: stub init\n");
    return 0;
}

void loop_shutdown(void)
{
    klog_write("loop: stub shutdown\n");
}
