#include "acpi_thermal.h"
#include "io.h"
#include "klog.h"

int acpi_thermal_init(void)
{
    klog_write("acpi_thermal: stub init\n");
    return 0;
}

void acpi_thermal_shutdown(void)
{
    klog_write("acpi_thermal: stub shutdown\n");
}
