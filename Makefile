CC = gcc
AS = as
LD = ld
AR = ar

GCC_INC := $(shell $(CC) -m32 -print-file-name=include)

# Regular kernel flags (no nostdinc to avoid breaking existing code)
CFLAGS = -m32 -ffreestanding -nostdlib -nostartfiles -nodefaultlibs \
         -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
         -mno-mmx -mno-sse -mno-sse2 \
         -O2 -Wall -Wextra -Werror \
         -I. \
         -Ikernel/core -Ikernel/display -Ikernel/drivers \
         -Ikernel/fs -Ikernel/shell -Ikernel/lib -Ikernel/net \
         -Ikernel/micropython \
         -Ikernel/micropython/ports/tos

# Relaxed flags for MicroPython core (uses -isystem for 32-bit glibc compat)
MPY_CFLAGS = -m32 -ffreestanding -nostdlib -nostartfiles -nodefaultlibs \
             -fno-stack-protector -fno-pic -fno-pie \
             -mno-mmx -mno-sse -mno-sse2 \
             -O2 -Wall -Wno-unused-variable -Wno-unused-function \
             -Wno-unused-parameter -Wno-sign-compare \
             -Wno-missing-field-initializers -Wno-implicit-fallthrough \
             -isystem /tmp/i386-linux-gnu \
             -I. \
             -Ikernel/micropython \
             -Ikernel/micropython/ports/tos \
             -Ikernel/micropython/genhdr

# Port files need kernel headers too
MPY_PORT_CFLAGS = $(MPY_CFLAGS) \
             -Ikernel/core -Ikernel/display -Ikernel/drivers \
             -Ikernel/fs -Ikernel/shell -Ikernel/lib -Ikernel/net
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
    kernel/lib/errno.o \
    kernel/lib/stdio.o \
    kernel/lib/stdlib.o \
    kernel/lib/ctype.o \
    kernel/fs/fs.o \
    kernel/fs/vfs.o \
    kernel/fs/elf.o \
    kernel/fs/syscall.o \
    kernel/core/serial.o \
    kernel/core/scheduler.o \
    kernel/shell/shell.o \
    kernel/drivers/pci.o \
    kernel/drivers/uhci.o \
    kernel/drivers/usb_keyboard.o \
    kernel/fs/ramfs.o \
    kernel/shell/tsharp.o \
    kernel/display/mouse.o \
    kernel/display/gui.o \
    kernel/net/net.o \
    kernel/net/nic.o \
    kernel/net/rtl8139.o \
    kernel/net/pcnet.o \
    kernel/net/e1000.o \
    kernel/net/arp.o \
    kernel/net/ip.o \
    kernel/net/icmp.o \
    kernel/net/udp.o \
    kernel/net/dns.o \
    kernel/net/tcp.o \
    kernel/net/http.o \
    kernel/micropython/ports/tos/tos_main.o \
    kernel/micropython/ports/tos/tos_hal.o \
    kernel/micropython/ports/tos/math_stubs.o \
    kernel/micropython/ports/tos/tos_stubs.o \
    kernel/micropython/shared/runtime/pyexec.o \
    kernel/micropython/shared/readline/readline.o

# MicroPython py/ core sources (exclude native/asm/persistent)
MPY_PY_SRCS := $(filter-out %/asmarm.c %/asmthumb.c %/asmxtensa.c %/asmrv32.c \
    %/emitnative.c %/emitinlinethumb.c %/emitinlinextensa.c %/emitinlinerrv32.c \
    %/persistentcode.c, $(wildcard kernel/micropython/py/*.c))
MPY_PY_OBJS := $(MPY_PY_SRCS:.c=.o)
KERNEL_OBJS += $(MPY_PY_OBJS)

PROGRAMS = programs/hello.elf

.PHONY: all clean run iso

all: tOS.iso

# MicroPython py/ core files use relaxed flags
kernel/micropython/py/%.o: kernel/micropython/py/%.c
	$(CC) $(MPY_CFLAGS) -c $< -o $@

kernel/micropython/shared/%.o: kernel/micropython/shared/%.c
	$(CC) $(MPY_CFLAGS) -c $< -o $@

kernel/micropython/ports/tos/%.o: kernel/micropython/ports/tos/%.c
	$(CC) $(MPY_PORT_CFLAGS) -c $< -o $@

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
