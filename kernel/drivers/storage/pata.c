#include "pata.h"
#include "io.h"
#include "klog.h"

int pata_init(void)
{
    klog_write("pata: stub init\n");
    return 0;
}

void pata_shutdown(void)
{
    klog_write("pata: stub shutdown\n");
}
