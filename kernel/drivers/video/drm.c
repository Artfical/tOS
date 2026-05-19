#include "drm.h"
#include "io.h"
#include "klog.h"

int drm_init(void)
{
    klog_write("drm: stub init\n");
    return 0;
}

void drm_shutdown(void)
{
    klog_write("drm: stub shutdown\n");
}
