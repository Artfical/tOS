#include "fbcon.h"
#include "io.h"
#include "klog.h"

int fbcon_init(void)
{
    klog_write("fbcon: stub init\n");
    return 0;
}

void fbcon_shutdown(void)
{
    klog_write("fbcon: stub shutdown\n");
}
