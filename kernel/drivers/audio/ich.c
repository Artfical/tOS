#include "ich.h"
#include "io.h"
#include "klog.h"

int ich_init(void)
{
    klog_write("ich: stub init\n");
    return 0;
}

void ich_shutdown(void)
{
    klog_write("ich: stub shutdown\n");
}
