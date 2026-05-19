#include "acpi_ec.h"
#include "io.h"
#include "klog.h"

int acpi_ec_init(void)
{
    klog_write("acpi_ec: stub init\n");
    return 0;
}

void acpi_ec_shutdown(void)
{
    klog_write("acpi_ec: stub shutdown\n");
}
