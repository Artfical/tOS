#include "i2c_eeprom.h"
#include "io.h"
#include "klog.h"

int i2c_eeprom_init(void)
{
    klog_write("i2c_eeprom: stub init\n");
    return 0;
}

void i2c_eeprom_shutdown(void)
{
    klog_write("i2c_eeprom: stub shutdown\n");
}
