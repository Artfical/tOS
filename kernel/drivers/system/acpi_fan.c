#include "acpi_fan.h"
#include "io.h"
#include "klog.h"

int acpi_fan_init(void)
{
    klog_write("acpi_fan: stub init\n");
    return 0;
}

void acpi_fan_shutdown(void)
{
    klog_write("acpi_fan: stub shutdown\n");
}
