#include "spi_nor.h"
#include "io.h"
#include "klog.h"

int spi_nor_init(void)
{
    klog_write("spi_nor: stub init\n");
    return 0;
}

void spi_nor_shutdown(void)
{
    klog_write("spi_nor: stub shutdown\n");
}
