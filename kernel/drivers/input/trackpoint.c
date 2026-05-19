#include "trackpoint.h"
#include "io.h"
#include "klog.h"

int trackpoint_init(void)
{
    klog_write("trackpoint: stub init\n");
    return 0;
}

void trackpoint_shutdown(void)
{
    klog_write("trackpoint: stub shutdown\n");
}
