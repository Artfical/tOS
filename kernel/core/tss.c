#include "tss.h"
#include "gdt.h"
#include "string.h"

static tss_t tss;

extern void gdt_set_tss(uint32_t tss_addr);

void tss_init(void)
{
    memset(&tss, 0, sizeof(tss));
    tss.ss0 = 0x10;
    tss.iomap = sizeof(tss);
    gdt_set_tss((uint32_t)&tss);
}

void tss_set_kernel_stack(uint32_t esp)
{
    tss.esp0 = esp;
}
