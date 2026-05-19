#include "i2c_tpm.h"
#include "io.h"
#include "klog.h"

int i2c_tpm_init(void)
{
    klog_write("i2c_tpm: stub init\n");
    return 0;
}

void i2c_tpm_shutdown(void)
{
    klog_write("i2c_tpm: stub shutdown\n");
}
