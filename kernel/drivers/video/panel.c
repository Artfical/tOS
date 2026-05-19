#include "panel.h"
#include "io.h"
#include "klog.h"

int panel_init(void)
{
    klog_write("panel: stub init\n");
    return 0;
}

void panel_shutdown(void)
{
    klog_write("panel: stub shutdown\n");
}
