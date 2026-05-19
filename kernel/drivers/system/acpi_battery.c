#include "acpi_battery.h"
#include "io.h"
#include "klog.h"

int acpi_battery_init(void)
{
    klog_write("acpi_battery: stub init\n");
    return 0;
}

void acpi_battery_shutdown(void)
{
    klog_write("acpi_battery: stub shutdown\n");
}
