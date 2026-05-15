#include "terminal.h"
#include "gdt.h"
#include "idt.h"
#include "isr.h"
#include "irq.h"
#include "keyboard.h"
#include "memory.h"
#include "fs.h"
#include "elf.h"
#include "syscall.h"
#include "shell.h"
#include "multiboot2.h"

extern uint32_t isr_stub_table[];

void kernel_main(uint32_t magic, uint32_t mb_info_addr)
{
    terminal_init();
    terminal_writestring("tOS v0.1.0 booting...\n");

    gdt_init();
    terminal_writestring("[OK] GDT initialized\n");

    idt_init();
    terminal_writestring("[OK] IDT initialized\n");

    isr_init();

    for (int i = 0; i < 48; i++) {
        idt_set_gate(i, isr_stub_table[i], 0x08, 0x8E);
    }

    terminal_writestring("[OK] ISR handlers set\n");

    irq_init();

    idt_set_gate(32, isr_stub_table[32], 0x08, 0x8E);
    idt_set_gate(33, isr_stub_table[33], 0x08, 0x8E);
    idt_set_gate(0x80, isr_stub_table[48], 0x08, 0x8E);

    terminal_writestring("[OK] IRQ handlers set\n");

    asm volatile("sti");

    uint32_t mem_upper = 0;
    if (magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
        terminal_writestring("[OK] Booted with Multiboot2\n");

        struct multiboot2_tag *tag = (struct multiboot2_tag *)(mb_info_addr + 8);
        for (;;) {
            if (tag->type == 0) break;

            if (tag->type == MULTIBOOT2_TAG_TYPE_MMAP) {
                struct multiboot2_tag_mmap *mmap = (struct multiboot2_tag_mmap *)tag;
                struct multiboot2_mmap_entry *entry = mmap->entries;
                while ((uint32_t)entry < (uint32_t)tag + tag->size) {
                    if (entry->type == 1 && entry->len > mem_upper * 1024ULL) {
                        uint64_t end = entry->addr + entry->len;
                        if (end > mem_upper * 1024ULL) {
                            mem_upper = (uint32_t)(end / 1024);
                        }
                    }
                    entry = (struct multiboot2_mmap_entry *)((uint32_t)entry + mmap->entry_size);
                }
            }

            if (tag->type == MULTIBOOT2_TAG_TYPE_MODULE) {
                struct multiboot2_tag_module *mod = (struct multiboot2_tag_module *)tag;
                terminal_writestring("[OK] Initrd module found at ");
                char buf[16];
                uint32_t addr = mod->mod_start;
                for (int i = 0; i < 8; i++) {
                    buf[7-i] = "0123456789ABCDEF"[addr & 0xF];
                    addr >>= 4;
                }
                buf[8] = '\0';
                terminal_writestring(buf);
                terminal_putchar('\n');

                fs_init(mod->mod_start, mod->mod_end - mod->mod_start);
            }

            uint32_t padding = tag->size % 8 ? 8 - (tag->size % 8) : 0;
            tag = (struct multiboot2_tag *)((uint32_t)tag + tag->size + padding);
        }
    }

    if (mem_upper == 0) mem_upper = 32768;
    memory_init(mem_upper);

    keyboard_init();
    terminal_writestring("[OK] Keyboard initialized\n");

    terminal_writestring("[OK] System ready\n");

    syscall_init();
    terminal_writestring("[OK] Syscalls initialized\n");

    shell_init();
    shell_run();
}
