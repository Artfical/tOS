#include "kexec.h"
#include "string.h"
#include "klog.h"
#include "bootsplash.h"
#include "uhci.h"

extern char kexec_tramp_start[], kexec_tramp_end[];

#define TABLE   0x7000
#define TRAMP   0x8000
#define MBINFO  0x9000
#define MBMOD   0x9100
#define MAXSEG  16

static uint32_t mem_lo = 640, mem_hi = 32768;

void kexec_set_memory(uint32_t mem_lower, uint32_t mem_upper) { mem_lo = mem_lower; mem_hi = mem_upper; }

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static int overlaps(uint32_t a, uint32_t al, uint32_t b, uint32_t bl) { return a < b + bl && b < a + al; }

void kexec_boot(const uint8_t *elf, uint32_t elf_len, const uint8_t *initrd, uint32_t initrd_len)
{
    if (elf_len < 52 || rd32(elf) != 0x464C457F || elf[4] != 1) { klog_write("kexec: not a 32-bit ELF\n"); return; }
    uint32_t entry = rd32(elf + 24), phoff = rd32(elf + 28);
    uint16_t phentsize = rd16(elf + 42), phnum = rd16(elf + 44);
    uint32_t seg_dst[MAXSEG], seg_src[MAXSEG], seg_len[MAXSEG];
    int n = 0;
    uint32_t lo = 0xFFFFFFFF, hi = 0;
    for (int i = 0; i < phnum; i++) {
        const uint8_t *ph = elf + phoff + (uint32_t)i * phentsize;
        if (ph + 32 > elf + elf_len || rd32(ph) != 1) continue;           /* PT_LOAD only */
        uint32_t off = rd32(ph + 4), paddr = rd32(ph + 12), filesz = rd32(ph + 16), memsz = rd32(ph + 20);
        if (off + filesz > elf_len || n + 2 > MAXSEG || paddr < 0x100000) { klog_write("kexec: odd segment\n"); return; }
        if (filesz) { seg_dst[n] = paddr; seg_src[n] = (uint32_t)(elf + off); seg_len[n] = filesz; n++; }
        if (memsz > filesz) { seg_dst[n] = paddr + filesz; seg_src[n] = 0; seg_len[n] = memsz - filesz; n++; }
        if (paddr < lo) lo = paddr;
        if (paddr + memsz > hi) hi = paddr + memsz;
    }
    if (!n) { klog_write("kexec: nothing to load\n"); return; }
    /* what the copy reads must not be where it writes: the image and the module both stay */
    if (overlaps((uint32_t)elf, elf_len, lo, hi - lo) || overlaps((uint32_t)initrd, initrd_len, lo, hi - lo)) {
        klog_write("kexec: the new image overlaps its own source\n");
        return;
    }

    /* the Multiboot information the new kernel gets: memory and its module */
    uint32_t *mb = (uint32_t *)MBINFO;
    memset(mb, 0, 0x100);
    mb[0] = (1 << 0) | (1 << 3);
    mb[1] = mem_lo;
    mb[2] = mem_hi;
    mb[5] = 1;
    mb[6] = MBMOD;
    uint32_t *mod = (uint32_t *)MBMOD;
    mod[0] = (uint32_t)initrd;
    mod[1] = (uint32_t)initrd + initrd_len;
    mod[2] = 0;
    mod[3] = 0;

    uint32_t *t = (uint32_t *)TABLE;
    t[0] = (uint32_t)n; t[1] = entry; t[2] = MBINFO; t[3] = 0;
    for (int i = 0; i < n; i++) { t[4 + i * 3] = seg_dst[i]; t[5 + i * 3] = seg_src[i]; t[6 + i * 3] = seg_len[i]; }
    memcpy((void *)TRAMP, kexec_tramp_start, (uint32_t)(kexec_tramp_end - kexec_tramp_start));

    klog_write("kexec: starting the new kernel\n");
    /* nothing may write into memory behind its back once it runs: the USB controller stops,
     * and the screen goes back to text so the new kernel finds the VGA state it expects */
    uhci_stop();
    bootsplash_end();
    __asm__ volatile("cli");
    ((void (*)(void))TRAMP)();
}
