#include "acpi_hotplug.h"
#include "io.h"
#include "klog.h"

int acpi_hotplug_init(void)
{
    klog_write("acpi_hotplug: stub init\n");
    return 0;
}

void acpi_hotplug_shutdown(void)
{
    klog_write("acpi_hotplug: stub shutdown\n");
}
