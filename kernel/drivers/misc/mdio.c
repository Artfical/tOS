#include "mdio.h"
#include "io.h"
#include "klog.h"

int mdio_init(void)
{
    klog_write("mdio: stub init\n");
    return 0;
}

void mdio_shutdown(void)
{
    klog_write("mdio: stub shutdown\n");
}
