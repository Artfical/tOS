#include "tss.h"
#include "gdt.h"
#include "string.h"

static tss_t tss;

void tss_init(void)
{
    memset(&tss, 0, sizeof(tss));
    tss.iomap = sizeof(tss);
    gdt_set_tss((uint64_t)&tss, sizeof(tss));
}

void tss_set_kernel_stack(uint64_t rsp)
{
    tss.rsp0 = rsp;
}
