#include "tcon.h"
#include "io.h"
#include "klog.h"

int tcon_init(void)
{
    klog_write("tcon: stub init\n");
    return 0;
}

void tcon_shutdown(void)
{
    klog_write("tcon: stub shutdown\n");
}
