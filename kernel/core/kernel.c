#define TOS_DISKTEST 1
#include "terminal.h"
#include "serial.h"
#include "gdt.h"
#include "idt.h"
#include "isr.h"
#include "irq.h"
#include "debugmon.h"
#include "keyboard.h"
#include "memory.h"
#include "paging.h"
#include "tss.h"
#include "usermode.h"
#include "fs.h"
#include "ramfs.h"
#include "syscall.h"
#include "shell.h"
#include "gui.h"
#include "wm.h"
#include "multiboot2.h"
#include "version.h"
#include "klog.h"
#include "net.h"
#include "micropython.h"
#include "scheduler.h"
#include "vga_font.h"
#include "vfs.h"
#include "pcspkr.h"
#include "ramfs.h"
#include "tfsk.h"
#include "installer.h"
#include "ata.h"
#include "io.h"
#include "ahci.h"
#include "nvme.h"
#include "usb_msd.h"
#include "blockdev.h"
#include "string.h"

#define MULTIBOOT_MAGIC 0x2BADB002

extern uint32_t isr_stub_table[];

static void busy_delay_ms(uint32_t ms)
{
    uint32_t needed = (uint32_t)((uint64_t)1193182 * ms / 1000);
    outb(0x43, 0x00);
    uint16_t start = inb(0x40);
    start |= inb(0x40) << 8;
    uint32_t elapsed = 0;
    uint16_t prev = start;
    while (elapsed < needed) {
        outb(0x43, 0x00);
        uint16_t curr = inb(0x40);
        curr |= inb(0x40) << 8;
        elapsed += (prev - curr) & 0xFFFF;
        prev = curr;
    }
}

static void show_boot_intro(void)
{
    static const char *logo[] = {
        "TTTTTTTTT  OOOOOOOOO  SSSSSSSSS",
        "    T     O         O S        ",
        "    T     O         O S        ",
        "    T     O         O SSSSSSSSS",
        "    T     O         O         S",
        "    T     O         O         S",
        "    T      OOOOOOOOO  SSSSSSSSS",
        0
    };

    terminal_setcolor(0x0B);
    terminal_clear();

    int row = 9;
    for (int i = 0; logo[i]; i++) {
        terminal_setpos(26, row++);
        terminal_writestring(logo[i]);
    }

    terminal_setpos(37, row + 1);
    terminal_setcolor(0x07);
    terminal_writestring("tOS");

    busy_delay_ms(2000);
    terminal_clear();
}

static void parse_multiboot2(uint32_t mb_info_addr, uint32_t *mem_upper, uint32_t *initrd_start, uint32_t *initrd_end)
{
    struct multiboot2_tag *tag = (struct multiboot2_tag *)(mb_info_addr + 8);
    for (;;) {
        if (tag->type == 0) break;

        if (tag->type == MULTIBOOT2_TAG_TYPE_MMAP) {
            struct multiboot2_tag_mmap *mmap = (struct multiboot2_tag_mmap *)tag;
            struct multiboot2_mmap_entry *entry = mmap->entries;
            while ((uint32_t)entry < (uint32_t)tag + tag->size) {
                if (entry->type == 1 && entry->len > *mem_upper * 1024ULL) {
                    uint64_t end = entry->addr + entry->len;
                    if (end > *mem_upper * 1024ULL) {
                        *mem_upper = (uint32_t)(end / 1024);
                    }
                }
                entry = (struct multiboot2_mmap_entry *)((uint32_t)entry + mmap->entry_size);
            }
        }

        if (tag->type == MULTIBOOT2_TAG_TYPE_MODULE) {
            struct multiboot2_tag_module *mod = (struct multiboot2_tag_module *)tag;
            *initrd_start = mod->mod_start;
            *initrd_end = mod->mod_end;
        }

        uint32_t padding = tag->size % 8 ? 8 - (tag->size % 8) : 0;
        tag = (struct multiboot2_tag *)((uint32_t)tag + tag->size + padding);
    }
}

static void parse_multiboot1(uint32_t mb_info_addr, uint32_t *mem_upper, uint32_t *initrd_start, uint32_t *initrd_end)
{
    uint32_t *info = (uint32_t *)mb_info_addr;
    uint32_t flags = info[0];

    if (flags & (1 << 0)) {
        *mem_upper = info[2];
    }

    if (flags & (1 << 3)) {
        uint32_t mods_count = info[5];
        uint32_t mods_addr = info[6];
        if (mods_count > 0) {
            uint32_t *mod = (uint32_t *)mods_addr;
            *initrd_start = mod[0];
            *initrd_end = mod[1];
        }
    }
}

void kernel_main(uint32_t magic, uint32_t mb_info_addr)
{
    serial_init();
    terminal_init();
    klog_init();
    serial_write(TOS_BOOT_STRING "\n");
    terminal_writestring(TOS_BOOT_STRING "\n");
    klog_write(TOS_BOOT_STRING "\n");

    gdt_init();
    terminal_writestring("[OK] GDT initialized\n");
    klog_write("[OK] GDT initialized\n");

    idt_init();
    terminal_writestring("[OK] IDT initialized\n");
    klog_write("[OK] IDT initialized\n");

    isr_init();
    for (int i = 0; i < 48; i++)
        idt_set_gate(i, isr_stub_table[i], 0x08, 0x8E);
    terminal_writestring("[OK] ISR handlers set\n");
    klog_write("[OK] ISR handlers set\n");

    irq_init();
    idt_set_gate(32, isr_stub_table[32], 0x08, 0x8E);
    idt_set_gate(33, isr_stub_table[33], 0x08, 0x8E);
    idt_set_gate(0x80, isr_stub_table[48], 0x08, 0xEE);
    terminal_writestring("[OK] IRQ handlers set\n");
    klog_write("[OK] IRQ handlers set\n");

    asm volatile("sti");

    /* Calibrate the TSC against real PIT ticks now, while IDT gate 32
     * still only fires on genuine hardware IRQ0 — scheduler_init()
     * later reinstalls that vector to also handle task_yield()'s
     * software self-yields, which would otherwise make calibration
     * count fake ticks too. See debugmon_calibrate_tsc(). */
    debugmon_calibrate_tsc();

    {
        /* Temporary boot-time self-check: print the calibrated TSC
         * frequency, then busy-wait for 100 more *real* hardware ticks
         * (ground truth, still unambiguous — scheduler hasn't taken
         * over IDT gate 32 yet) and compare that ~1000ms against what
         * debugmon_uptime_ms()'s TSC arithmetic reports over the same
         * span. A silent calibration failure (falling back to the old
         * yield-driven fast clock, or bad tsc_per_ms) is otherwise
         * invisible except as network commands mysteriously failing
         * fast. */
        uint32_t tpm = debugmon_get_tsc_per_ms();
        uint32_t t0_ground = debugmon_get_tick_count();
        uint32_t t0_tsc = debugmon_uptime_ms();
        while (debugmon_get_tick_count() - t0_ground < 100) { }
        uint32_t t1_tsc = debugmon_uptime_ms();

        char line[96]; int lp = 0;
        char nb[12]; int ni;
        const char *p1 = "[DBG] tsc_per_ms=";
        while (*p1) line[lp++] = *p1++;
        ni = 11; nb[11] = 0; { uint32_t v = tpm; if (v==0) nb[--ni]='0'; else while (v>0 && ni>0){nb[--ni]='0'+v%10; v/=10;} }
        while (nb[ni]) line[lp++] = nb[ni++];
        const char *p2 = " ground_truth_ms=1000 tsc_measured_ms=";
        while (*p2) line[lp++] = *p2++;
        ni = 11; nb[11] = 0; { uint32_t v = t1_tsc - t0_tsc; if (v==0) nb[--ni]='0'; else while (v>0 && ni>0){nb[--ni]='0'+v%10; v/=10;} }
        while (nb[ni]) line[lp++] = nb[ni++];
        line[lp++] = '\n';
        line[lp] = '\0';
        terminal_writestring(line);
        klog_write(line);
    }

    pcspkr_init();

    show_boot_intro();

    uint32_t mem_upper = 0;
    uint32_t initrd_start = 0;
    uint32_t initrd_end = 0;

    if (magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
        terminal_writestring("[OK] Booted with Multiboot2\n");
        klog_write("[OK] Booted with Multiboot2\n");
        parse_multiboot2(mb_info_addr, &mem_upper, &initrd_start, &initrd_end);
    } else if (magic == MULTIBOOT_MAGIC) {
        terminal_writestring("[OK] Booted with Multiboot1\n");
        klog_write("[OK] Booted with Multiboot1\n");
        parse_multiboot1(mb_info_addr, &mem_upper, &initrd_start, &initrd_end);
    } else {
        terminal_writestring("[WARN] Unknown bootloader\n");
        klog_write("[WARN] Unknown bootloader\n");
    }

    int have_initrd = (initrd_start && initrd_end > initrd_start);
    if (have_initrd) {
        terminal_writestring("[OK] Initrd module found\n");
        klog_write("[OK] Initrd module found\n");
    } else {
        terminal_writestring("[WARN] No initrd module found\n");
        klog_write("[WARN] No initrd module found\n");
    }

    if (mem_upper == 0) mem_upper = 32768;
    memory_init(mem_upper, have_initrd ? initrd_end : 0);

    paging_init();
    tss_init();
    usermode_init();

    ramfs_init();
    terminal_writestring("[OK] Ramfs initialized\n");
    klog_write("[OK] Ramfs initialized\n");

    /* Must run after ramfs_init() (the root inode and inode table don't
     * exist before that) and after memory_init() (file data is copied
     * in via malloc) — importing earlier, against an uninitialized
     * ramfs that ramfs_init() then resets, silently dropped every file
     * the initrd ever carried. */
    if (have_initrd) {
        ramfs_import_initrd(initrd_start, initrd_end - initrd_start);
    }

    vfs_init();
    ramfs_mount_vfs();
    terminal_writestring("[OK] VFS initialized\n");
    klog_write("[OK] VFS initialized\n");

    blockdev_init();

    int ata_count = ata_init();
    terminal_writestring("[OK] ATA initialized (");
    terminal_putchar('0' + ata_count);
    terminal_writestring(" devices)\n");
    klog_write("[OK] ATA initialized\n");
    for (int ai = 0; ai < ata_device_count; ai++) {
        if (ata_devices[ai].present)
            blockdev_register_ata(&ata_devices[ai]);
    }

    static ahci_hba_t ahci_hba_instance;
    if (ahci_init(&ahci_hba_instance) == 0) {
        terminal_writestring("[OK] AHCI initialized (");
        terminal_putchar('0' + ahci_hba_instance.port_count);
        terminal_writestring(" ports)\n");
        klog_write("[OK] AHCI initialized\n");
        for (int pi = 0; pi < AHCI_MAX_PORTS; pi++) {
            if (ahci_hba_instance.ports[pi])
                blockdev_register_ahci(&ahci_hba_instance, pi, ahci_hba_instance.sectors[pi]);
        }
#ifdef TOS_DISKTEST
        {
            static char diskbuf[512];
            for (int dp = 0; dp < AHCI_MAX_PORTS; dp++) {
                if (!ahci_hba_instance.ports[dp]) continue;
                memset(diskbuf, 0, sizeof(diskbuf));
                if (ahci_read(&ahci_hba_instance, dp, 0, 1, diskbuf) == 0) {
                    diskbuf[40] = '\0';
                    terminal_writestring("[DISKTEST] AHCI LBA0: ");
                    terminal_writestring(diskbuf);
                    terminal_writestring("\n");
                    klog_write("[DISKTEST] AHCI LBA0: ");
                    klog_write(diskbuf);
                    klog_write("\n");
                } else {
                    terminal_writestring("[DISKTEST] AHCI read FAILED\n");
                    klog_write("[DISKTEST] AHCI read FAILED\n");
                }
            }
        }
#endif
    } else {
        terminal_writestring("[INFO] No AHCI controller found\n");
        klog_write("[INFO] No AHCI controller found\n");
    }

    static nvme_device_t nvme_dev_instance;
    if (nvme_init(&nvme_dev_instance) == 0) {
        terminal_writestring("[OK] NVMe initialized\n");
        klog_write("[OK] NVMe initialized\n");
        blockdev_register_nvme(&nvme_dev_instance);
#ifdef TOS_DISKTEST
        {
            static char diskbuf[512];
            memset(diskbuf, 0, sizeof(diskbuf));
            if (nvme_read(&nvme_dev_instance, 0, 1, diskbuf) == 0) {
                diskbuf[40] = '\0';
                terminal_writestring("[DISKTEST] NVMe LBA0: ");
                terminal_writestring(diskbuf);
                terminal_writestring("\n");
                klog_write("[DISKTEST] NVMe LBA0: ");
                klog_write(diskbuf);
                klog_write("\n");
            } else {
                terminal_writestring("[DISKTEST] NVMe read FAILED\n");
                klog_write("[DISKTEST] NVMe read FAILED\n");
            }
        }
#endif
    } else {
        terminal_writestring("[INFO] No NVMe controller found\n");
        klog_write("[INFO] No NVMe controller found\n");
    }

    static usb_msd_device_t usb_msd_instance;
    if (usb_msd_init(&usb_msd_instance) == 0) {
        terminal_writestring("[OK] USB mass storage initialized\n");
        klog_write("[OK] USB mass storage initialized\n");
        blockdev_register_usb(&usb_msd_instance);
#ifdef TOS_DISKTEST
        {
            static char diskbuf[512];
            memset(diskbuf, 0, sizeof(diskbuf));
            if (usb_msd_read(&usb_msd_instance, 0, 1, diskbuf) == 0) {
                diskbuf[40] = '\0';
                terminal_writestring("[DISKTEST] USB-MSD LBA0: ");
                terminal_writestring(diskbuf);
                terminal_writestring("\n");
                klog_write("[DISKTEST] USB-MSD LBA0: ");
                klog_write(diskbuf);
                klog_write("\n");
            } else {
                terminal_writestring("[DISKTEST] USB-MSD read FAILED\n");
                klog_write("[DISKTEST] USB-MSD read FAILED\n");
            }
        }
#endif
    } else {
        terminal_writestring("[INFO] No USB mass storage device found\n");
        klog_write("[INFO] No USB mass storage device found\n");
    }

    static tfsk_t tfs_instance;
    if (installer_check_installed()) {
        terminal_writestring("[OK] tFS disk detected, mounting...\n");
        klog_write("[OK] tFS disk detected, mounting...\n");
        if (tfsk_probe_and_mount(&tfs_instance) == 0) {
            tfsk_mount_vfs(&tfs_instance, "/mnt");
            terminal_writestring("[OK] tFS mounted at /mnt\n");
            klog_write("[OK] tFS mounted at /mnt\n");
        } else {
            terminal_writestring("[WARN] tFS mount failed\n");
            klog_write("[WARN] tFS mount failed\n");
        }
    } else {
        terminal_writestring("[INFO] No tFS disk found. Run installer to set up.\n");
        klog_write("[INFO] No tFS disk found.\n");
    }

    keyboard_init();
    terminal_writestring("[OK] Keyboard initialized\n");
    klog_write("[OK] Keyboard initialized\n");

    /* Must happen before anything ever touches Bochs/VBE (DOOM/vgatest/
     * the 3d rasterizer) -- their text-mode restore afterward needs a
     * known-good copy of the character glyph bitmaps to reload into
     * VGA plane 2, since that VRAM region doesn't survive a VBE
     * session intact. vga_font_load_turkish() below already captures
     * this as a side effect for the TR-Q layout, but the US layout
     * (the more common choice) skipped it entirely until now. */
    vga_font_capture_base();

    terminal_writestring("\nKeyboard layout: [US] (1) or [TR-Q] (2) ? ");
    for (;;) {
        char c = keyboard_getchar();
        if (c == '1') {
            keyboard_set_layout(KBD_LAYOUT_US);
            terminal_writestring("US\n");
            break;
        } else if (c == '2') {
            keyboard_set_layout(KBD_LAYOUT_TRQ);
            terminal_writestring("TR-Q\n");
            vga_font_load_turkish();
            break;
        }
    }

    if (!tfs_instance.mounted && ata_device_count > 0) {
        if (!installer_check_installed()) {
            terminal_writestring("\n");
            installer_run();
            terminal_setcolor(0x07);
            terminal_clear();
        }
        if (!tfs_instance.mounted && installer_check_installed()) {
            if (tfsk_probe_and_mount(&tfs_instance) == 0) {
                tfsk_mount_vfs(&tfs_instance, "/mnt");
                terminal_writestring("[OK] tFS mounted at /mnt\n");
                klog_write("[OK] tFS mounted at /mnt\n");
            }
        }
    }

    int sel = 0;
    terminal_writestring("\nGUI? <[No] Yes>  (arrows/y/n, Enter)");
    for (;;) {
        int k = keyboard_yesno();
        if (k == 0) { sel = 0; terminal_writestring("\rGUI? <[No] Yes>  (arrows/y/n, Enter)"); }
        else if (k == 1) { sel = 1; terminal_writestring("\rGUI? < No [Yes]> (arrows/y/n, Enter)"); }
        else { terminal_putchar('\n'); break; }
    }
    if (sel) {
        terminal_writestring("[OK] GUI initialized\n");
    } else {
        terminal_writestring("CLI mode\n");
    }

    terminal_writestring("[OK] System ready\n");
    klog_write("[OK] System ready\n");

    syscall_init();
    terminal_writestring("[OK] Syscalls initialized\n");
    klog_write("[OK] Syscalls initialized\n");

    net_init();

    micropython_init();

    scheduler_init();
    terminal_writestring("[OK] Scheduler initialized\n");
    klog_write("[OK] Scheduler initialized\n");

    shell_init();

    if (sel) {
        gui_init();
        wm_run();
    } else {
        shell_run();
    }
}
