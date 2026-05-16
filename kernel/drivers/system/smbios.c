#include "smbios.h"
#include "string.h"
static uint32_t smbios_addr = 0;
int smbios_init(void)
{
    for (uint32_t addr = SMBIOS_ENTRY_POINT; addr < SMBIOS_ENTRY_POINT + SMBIOS_SEARCH_SIZE; addr += 16) {
        if (*(uint32_t *)addr == 0x5F4D535F || *(uint32_t *)addr == 0x5F534D5F) {
            smbios_addr = addr;
            return 0;
        }
    }
    return -1;
}
smbios_header_t *smbios_find_entry(uint8_t type)
{
    (void)type;
    return NULL;
}
const char *smbios_get_string(smbios_header_t *header, uint8_t index)
{
    (void)header;
    (void)index;
    return NULL;
}
