CC = gcc
AS = as
LD = ld
AR = ar

CFLAGS = -m32 -ffreestanding -nostdlib -nostartfiles -nodefaultlibs \
         -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
         -mno-mmx -mno-sse -mno-sse2 \
         -O2 -Wall -Wextra -Werror \
         -I.
LDFLAGS = -m elf_i386 -T kernel/linker.ld
ASFLAGS = --32

KERNEL_OBJS = \
    kernel/boot.o \
    kernel/isr_stubs.o \
    kernel/kernel.o \
    kernel/terminal.o \
    kernel/string.o \
    kernel/gdt.o \
    kernel/idt.o \
    kernel/isr.o \
    kernel/irq.o \
    kernel/keyboard.o \
    kernel/memory.o \
    kernel/fs.o \
    kernel/elf.o \
    kernel/syscall.o \
    kernel/serial.o \
    kernel/shell.o \
    kernel/pci.o \
    kernel/uhci.o \
    kernel/usb_keyboard.o \
    kernel/ramfs.o \
    kernel/tsharp.o \
    kernel/mouse.o \
    kernel/gui.o

PROGRAMS = programs/hello.elf

.PHONY: all clean run iso

all: tOS.iso

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.s
	$(AS) $(ASFLAGS) $< -o $@

kernel/boot.o: kernel/boot.s
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
