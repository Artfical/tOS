#include "vlan.h"
#include "net.h"
#include "string.h"
#include "terminal.h"

static uint16_t vlan_table[VLAN_MAX];
static int      vlan_count = 0;

int vlan_add(uint16_t vid)
{
    if (vid > 4094) return -1;
    for (int i = 0; i < vlan_count; i++)
        if (vlan_table[i] == vid) return 0; /* already present */
    if (vlan_count >= VLAN_MAX) return -1;
    vlan_table[vlan_count++] = vid;
    return 0;
}

int vlan_remove(uint16_t vid)
{
    for (int i = 0; i < vlan_count; i++) {
        if (vlan_table[i] == vid) {
            vlan_table[i] = vlan_table[--vlan_count];
            return 0;
        }
    }
    return -1;
}

void vlan_list(void)
{
    if (vlan_count == 0) {
        terminal_writestring("VLAN: no VLANs configured (accept all)\n");
        return;
    }
    terminal_writestring("VLAN table:\n");
    for (int i = 0; i < vlan_count; i++) {
        terminal_writestring("  vid=");
        /* print decimal VID */
        uint16_t v = vlan_table[i];
        char buf[8]; int j = 7; buf[7] = '\0';
        if (v == 0) { buf[6] = '0'; j = 6; }
        else { while (v > 0 && j > 0) { buf[--j] = '0' + (v % 10); v /= 10; } }
        terminal_writestring(buf + j);
        terminal_writestring("\n");
    }
}

int vlan_allowed(uint16_t vid)
{
    if (vlan_count == 0) return 1; /* no filter configured — accept all */
    for (int i = 0; i < vlan_count; i++)
        if (vlan_table[i] == vid) return 1;
    return 0;
}

uint16_t vlan_strip(uint8_t *frame, int *len, uint16_t *out_vid)
{
    if (*len < 16) return 0;
    /* EtherType is at bytes 12-13 (after 6+6 MAC) */
    uint16_t etype = (uint16_t)((frame[12] << 8) | frame[13]);
    if (etype != ETHERTYPE_VLAN) return 0;

    vlan_tag_t *tag = (vlan_tag_t *)(frame + 12);
    uint16_t tci        = ntohs(tag->tci);
    uint16_t inner_type = ntohs(tag->inner_type);
    uint16_t vid        = VLAN_VID(tci);

    if (out_vid) *out_vid = vid;

    /*
     * Remove the 4-byte tag: shift bytes 0..11 forward by 4,
     * then overwrite bytes 12-13 with inner_type.
     */
    memmove(frame + 4, frame, 12);
    frame[4 + 12]     = (uint8_t)(inner_type >> 8);
    frame[4 + 13]     = (uint8_t)(inner_type & 0xFF);
    /* now the real frame starts 4 bytes in — shift back to offset 0 */
    memmove(frame, frame + 4, *len - 4);
    *len -= 4;

    return inner_type;
}

void vlan_insert(uint8_t *frame, int *len, uint16_t vid, uint8_t pcp)
{
    /* make room: shift everything from byte 12 onwards right by 4 */
    memmove(frame + 16, frame + 12, *len - 12);
    uint16_t tci        = (uint16_t)(((pcp & 0x07) << 13) | (vid & 0x0FFF));
    uint16_t inner_type = (uint16_t)((frame[12 + 4] << 8) | frame[13 + 4]);
    /* bytes 12-13: VLAN EtherType */
    frame[12] = 0x81; frame[13] = 0x00;
    /* bytes 14-15: TCI */
    frame[14] = (uint8_t)(tci >> 8); frame[15] = (uint8_t)(tci & 0xFF);
    /* bytes 16-17: original EtherType (already shifted) */
    (void)inner_type;
    *len += 4;
}
