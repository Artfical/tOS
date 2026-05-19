#include "eeprom.h"
#include "io.h"
#include "klog.h"

int eeprom_init(void)
{
    klog_write("eeprom: stub init\n");
    return 0;
}

void eeprom_shutdown(void)
{
    klog_write("eeprom: stub shutdown\n");
}
