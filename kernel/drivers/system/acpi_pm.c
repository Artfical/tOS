#include "acpi_pm.h"
#include "io.h"
#include "klog.h"

int acpi_pm_init(void)
{
    klog_write("acpi_pm: stub init\n");
    return 0;
}

void acpi_pm_shutdown(void)
{
    klog_write("acpi_pm: stub shutdown\n");
}
