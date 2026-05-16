#include "bios32.h"
#include "string.h"
static bios32_t *bios32_find(void)
{
    for (uint32_t addr = 0x000E0000; addr < 0x00100000; addr += 16) {
        bios32_t *b = (bios32_t *)addr;
        if (b->signature[0] == '_' && b->signature[1] == '3' &&
            b->signature[2] == '2' && b->signature[3] == '_') {
            uint8_t sum = 0;
            for (int i = 0; i < b->length; i++) sum += ((uint8_t *)b)[i];
            if (sum == 0) return b;
        }
    }
    return NULL;
}
static bios32_t *bios32_entry = NULL;
int bios32_init(void)
{
    bios32_entry = bios32_find();
    return bios32_entry ? 0 : -1;
}
void *bios32_find_service(uint32_t service)
{
    (void)service;
    return NULL;
}
