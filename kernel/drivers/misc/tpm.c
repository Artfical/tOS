#include "tpm.h"
#include "io.h"
#include "klog.h"

int tpm_init(void)
{
    klog_write("tpm: stub init\n");
    return 0;
}

void tpm_shutdown(void)
{
    klog_write("tpm: stub shutdown\n");
}
