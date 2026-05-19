#include "spi_nand.h"
#include "io.h"
#include "klog.h"

int spi_nand_init(void)
{
    klog_write("spi_nand: stub init\n");
    return 0;
}

void spi_nand_shutdown(void)
{
    klog_write("spi_nand: stub shutdown\n");
}
