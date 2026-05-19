#include "pci_hotplug.h"
#include "io.h"
#include "klog.h"

int pci_hotplug_init(void)
{
    klog_write("pci_hotplug: stub init\n");
    return 0;
}

void pci_hotplug_shutdown(void)
{
    klog_write("pci_hotplug: stub shutdown\n");
}
