#include "terminal.h"
#include "serial.h"
#include "gdt.h"
#include "idt.h"
#include "isr.h"
#include "irq.h"
#include "keyboard.h"
#include "memory.h"
#include "paging.h"
#include "tss.h"
#include "usermode.h"
#include "fs.h"
#include "ramfs.h"
#include "elf.h"
#include "syscall.h"
#include "shell.h"
#include "gui.h"
#include "multiboot2.h"
#include "version.h"
#include "klog.h"
#include "net.h"
#include "micropython.h"
#include "scheduler.h"
#include "vga_font.h"
#include "vfs.h"
#include "ramfs.h"
#include "tfsk.h"
#include "installer.h"
#include "ata.h"
#include "io.h"

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
        *mem_upper = info[3];
    }

    if ((flags & (1 << 3)) && (flags & (1 << 6))) {
        uint32_t mods_count = info[8];
        uint32_t mods_addr = info[9];
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

    if (initrd_start && initrd_end > initrd_start) {
        terminal_writestring("[OK] Initrd module found\n");
        klog_write("[OK] Initrd module found\n");
        ramfs_import_initrd(initrd_start, initrd_end - initrd_start);
    } else {
        terminal_writestring("[WARN] No initrd module found\n");
        klog_write("[WARN] No initrd module found\n");
    }

    if (mem_upper == 0) mem_upper = 32768;
    memory_init(mem_upper);

    paging_init();
    tss_init();
    usermode_init();

    ramfs_init();
    terminal_writestring("[OK] Ramfs initialized\n");
    klog_write("[OK] Ramfs initialized\n");

    vfs_init();
    ramfs_mount_vfs();
    terminal_writestring("[OK] VFS initialized\n");
    klog_write("[OK] VFS initialized\n");

    int ata_count = ata_init();
    terminal_writestring("[OK] ATA initialized (");
    terminal_putchar('0' + ata_count);
    terminal_writestring(" devices)\n");
    klog_write("[OK] ATA initialized\n");

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
        gui_init();
        terminal_set_y_offset(GUI_TERM_ROW);
        terminal_clear();
        gui_draw_titlebar();
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

    shell_init();
    shell_run();
}
