CC = gcc
AS = as
LD = ld
AR = ar

CFLAGS = -m32 -ffreestanding -nostdlib -nostartfiles -nodefaultlibs \
         -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
         -mno-mmx -mno-sse -mno-sse2 \
         -O2 -Wall -Wextra -Werror \
         -I. \
         -Ikernel/core -Ikernel/display -Ikernel/drivers \
         -Ikernel/fs -Ikernel/shell -Ikernel/lib
LDFLAGS = -m elf_i386 -T kernel/boot/linker.ld
ASFLAGS = --32

KERNEL_OBJS = \
    kernel/boot/boot.o \
    kernel/boot/isr_stubs.o \
    kernel/core/kernel.o \
    kernel/display/terminal.o \
    kernel/lib/string.o \
    kernel/core/gdt.o \
    kernel/core/idt.o \
    kernel/core/isr.o \
    kernel/core/irq.o \
    kernel/drivers/keyboard.o \
    kernel/lib/memory.o \
    kernel/fs/fs.o \
    kernel/fs/elf.o \
    kernel/fs/syscall.o \
    kernel/core/serial.o \
    kernel/shell/shell.o \
    kernel/drivers/pci.o \
    kernel/drivers/uhci.o \
    kernel/drivers/usb_keyboard.o \
    kernel/fs/ramfs.o \
    kernel/shell/tsharp.o \
    kernel/display/mouse.o \
    kernel/display/gui.o

PROGRAMS = programs/hello.elf

.PHONY: all clean run iso

all: tOS.iso

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.s
	$(AS) $(ASFLAGS) $< -o $@

kernel/boot/boot.o: kernel/boot/boot.s
	$(CC) $(CFLAGS) -x assembler-with-cpp -c $< -o $@

kernel/tOS.elf: $(KERNEL_OBJS)
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

programs/hello.elf: programs/hello.c
	$(CC) -m32 -ffreestanding -nostdlib -nostartfiles -fno-builtin -fno-stack-protector \
	      -O2 -Wall -static -T programs/program.ld -I. -o $@ $<

initrd.tar: $(PROGRAMS)
	tar cf $@ --format=ustar $^

tOS.iso: kernel/tOS.elf initrd.tar
	mkdir -p iso/boot/grub
	cp kernel/tOS.elf iso/boot/
	cp initrd.tar iso/boot/
	cp boot/grub/grub.cfg iso/boot/grub/
	LD_LIBRARY_PATH="/tmp/opencode/xorriso_libs/usr/lib/x86_64-linux-gnu:/tmp/opencode/xorriso_fulldir/usr/lib/x86_64-linux-gnu" \
	PATH="/tmp/opencode/xorriso_extracted/usr/bin:$$PATH" \
	grub-mkrescue -o $@ iso

run: tOS.iso
	qemu-system-x86_64 -cdrom tOS.iso -m 256M -serial stdio 2>/dev/null || \
	qemu-system-x86_64 -cdrom tOS.iso -m 256M

clean:
	rm -f $(KERNEL_OBJS) kernel/tOS.elf initrd.tar tOS.iso programs/hello.elf
	rm -rf iso
