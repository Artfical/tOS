#include "emu10k1.h"
#include "io.h"
#include "klog.h"

int emu10k1_init(void)
{
    klog_write("emu10k1: stub init\n");
    return 0;
}

void emu10k1_shutdown(void)
{
    klog_write("emu10k1: stub shutdown\n");
}
