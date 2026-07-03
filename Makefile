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
         -Ikernel/core -Ikernel/display \
         -Ikernel/fs -Ikernel/shell -Ikernel/shell/commands -Ikernel/lib -Ikernel/net \
          -Ikernel/drivers/include -Ikernel/drivers/bus -Ikernel/drivers/storage \
          -Ikernel/drivers/net -Ikernel/drivers/usb -Ikernel/drivers/audio \
          -Ikernel/drivers/video -Ikernel/drivers/input -Ikernel/drivers/system \
          -Ikernel/drivers/misc -Ikernel/micropython \
         -Ikernel/micropython/ports/tos \
         -Ikernel/audio

# Relaxed flags for MicroPython core (uses -isystem for 32-bit glibc compat)
MPY_CFLAGS = -m32 -ffreestanding -nostdlib -nostartfiles -nodefaultlibs \
             -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
             -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
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
             -Ikernel/core -Ikernel/display \
             -Ikernel/fs -Ikernel/shell -Ikernel/lib -Ikernel/net \
              -Ikernel/drivers/include -Ikernel/drivers/bus -Ikernel/drivers/storage \
              -Ikernel/drivers/net -Ikernel/drivers/usb -Ikernel/drivers/audio \
              -Ikernel/drivers/video -Ikernel/drivers/input -Ikernel/drivers/system \
              -Ikernel/drivers/misc
LDFLAGS = -m elf_i386 -T kernel/boot/linker.ld
ASFLAGS = --32

# Auto-discover driver sources
DRIVER_SRCS := $(wildcard kernel/drivers/bus/*.c kernel/drivers/storage/*.c \
    kernel/drivers/net/*.c kernel/drivers/usb/*.c kernel/drivers/audio/*.c \
    kernel/drivers/video/*.c kernel/drivers/input/*.c kernel/drivers/system/*.c \
    kernel/drivers/misc/*.c)
DRIVER_OBJS := $(DRIVER_SRCS:.c=.o)

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
    kernel/lib/memory.o \
    kernel/lib/errno.o \
    kernel/lib/stdio.o \
    kernel/lib/stdlib.o \
    kernel/lib/ctype.o \
    kernel/lib/udivdi3.o \
    kernel/fs/fs.o \
    kernel/fs/vfs.o \
    kernel/fs/fsbridge.o \
    kernel/fs/elf.o \
    kernel/fs/syscall.o \
    kernel/core/serial.o \
    kernel/core/debugmon.o \
    kernel/core/sound.o \
    kernel/core/scheduler.o \
    kernel/core/paging.o \
    kernel/core/tss.o \
    kernel/core/usermode.o \
    kernel/shell/shell.o \
    kernel/shell/commands/cmd_file.o \
    kernel/shell/commands/cmd_fs.o \
    kernel/shell/commands/cmd_sys.o \
    kernel/shell/commands/cmd_man.o \
    kernel/shell/commands/cmd_util.o \
    kernel/shell/commands/cmd_net.o \
    kernel/shell/commands/cmd_htop.o \
    kernel/shell/commands/cmd_disk.o \
    kernel/shell/commands/cmd_archive.o \
    kernel/core/klog.o \
    kernel/fs/ramfs.o \
    kernel/fs/tfsk.o \
    kernel/fs/fat16.o \
    kernel/fs/fat32.o \
    kernel/fs/exfat.o \
    kernel/fs/ext2.o \
    kernel/fs/ext3.o \
    kernel/fs/ext4.o \
    kernel/fs/ntfs.o \
    kernel/fs/btrfs.o \
    kernel/fs/xfs.o \
    kernel/fs/zfs.o \
    kernel/fs/apfs.o \
    kernel/fs/diskops.o \
    kernel/fs/tarfmt.o \
    kernel/fs/zipfmt.o \
    kernel/fs/installer.o \
    kernel/shell/tsharp.o \
    kernel/shell/tos_api.o \
    kernel/display/gui.o \
    kernel/display/wm.o \
    kernel/display/notepad.o \
    kernel/display/clock.o \
    kernel/display/about.o \
    kernel/display/diskmgr.o \
    kernel/display/calculator.o \
    kernel/display/filemgr.o \
    kernel/display/paint.o \
    kernel/display/png.o \
    kernel/display/viewer.o \
    kernel/display/taskmgr.o \
    kernel/net/net.o \
    kernel/net/nic.o \
    kernel/net/rtl8139.o \
    kernel/net/pcnet.o \
    kernel/net/e1000.o \
    kernel/net/virtio_net.o \
    kernel/net/ne2000.o \
    kernel/net/arp.o \
    kernel/net/ip.o \
    kernel/net/icmp.o \
    kernel/net/udp.o \
    kernel/net/dns.o \
    kernel/net/tcp.o \
    kernel/net/sctp.o \
    kernel/net/dccp.o \
    kernel/net/udplite.o \
    kernel/net/ip6.o \
    kernel/net/icmpv6.o \
    kernel/net/ipsec.o \
    kernel/net/vlan.o \
    kernel/net/bridge.o \
    kernel/net/bonding.o \
    kernel/net/ipx.o \
    kernel/net/route.o \
    kernel/net/fw.o \
    kernel/net/gre.o \
    kernel/net/ipip.o \
    kernel/net/chacha20.o \
    kernel/net/wgtun.o \
    kernel/net/sha256.o \
    kernel/net/aes.o \
    kernel/net/bignum.o \
    kernel/net/tls.o \
    kernel/net/https.o \
    kernel/net/http.o \
    kernel/audio/audio.o \
    kernel/audio/wav_decoder.o \
    kernel/audio/mp3_decoder.o \
    kernel/audio/aac_decoder.o \
        kernel/audio/demo_song_embed.o \
    kernel/display/mediaplayer.o \
    kernel/display/netmon.o \
    kernel/display/snake.o \
    kernel/micropython/ports/tos/tos_main.o \
    kernel/micropython/ports/tos/tos_hal.o \
    kernel/micropython/ports/tos/math_stubs.o \
    kernel/micropython/ports/tos/tos_stubs.o \
    kernel/micropython/ports/tos/modtos.o \
    kernel/micropython/ports/tos/modtosgui.o \
    kernel/micropython/ports/tos/modos.o \
    kernel/micropython/ports/tos/modtime.o \
    kernel/micropython/ports/tos/modjson.o \
    kernel/micropython/ports/tos/modsocket.o \
    kernel/micropython/ports/tos/runfile.o \
    kernel/micropython/shared/runtime/pyexec.o \
    kernel/micropython/shared/readline/readline.o

KERNEL_OBJS += $(DRIVER_OBJS)

# MicroPython py/ core sources (exclude native/asm/persistent)
MPY_PY_SRCS := $(filter-out %/asmarm.c %/asmthumb.c %/asmxtensa.c %/asmrv32.c \
    %/emitnative.c %/emitinlinethumb.c %/emitinlinextensa.c %/emitinlinerrv32.c \
    %/persistentcode.c, $(wildcard kernel/micropython/py/*.c))
MPY_PY_OBJS := $(MPY_PY_SRCS:.c=.o)
KERNEL_OBJS += $(MPY_PY_OBJS)

PROGRAMS = programs/hello.elf programs/tosgui_demo.py lib/libc.so programs/hello_dyn.elf

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

kernel/audio/demo_song_embed.o: kernel/audio/demo_song_embed.s kernel/audio/demo_song.wav
	$(AS) $(ASFLAGS) $< -o $@

kernel/audio/demo_song.wav: gen_demo_song.py
	python3 gen_demo_song.py || true

kernel/boot/boot.o: kernel/boot/boot.s
	$(CC) $(CFLAGS) -x assembler-with-cpp -c $< -o $@

# No automatic header dependency tracking (no -MMD/.d files) — force every
# kernel .c to recompile whenever version.h changes, so a version bump can
# never leave a stale TOS_VERSION/TOS_BOOT_STRING baked into an old .o
# (this bit us once: kernel.c's boot banner stayed on 0.9.52 for a dozen
# releases because nothing forced it to rebuild).
CORE_C_OBJS := $(patsubst %.c,%.o,$(wildcard kernel/core/*.c kernel/display/*.c kernel/shell/*.c kernel/shell/commands/*.c))
$(CORE_C_OBJS): kernel/core/version.h

kernel/tOS.elf: $(KERNEL_OBJS)
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

programs/hello.elf: programs/hello.c
	$(CC) -m32 -ffreestanding -nostdlib -nostartfiles -fno-builtin -fno-stack-protector \
	      -fno-pic -fno-pie \
	      -O2 -Wall -static -T programs/program.ld -I. -o $@ $<

# Dynamic-linking userland: a minimal shared libc (kernel/fs/elf.c's
# elf_load_dynamic() resolves DT_NEEDED against /lib/<soname>) plus a
# test program that imports puts/printf/exit from it via PLT/GOT
# relocations.
lib/libc.so: userland/libc/tlibc.c
	mkdir -p lib
	$(CC) -m32 -fPIC -nostdlib -fno-stack-protector -c $< -o lib/tlibc.o
	$(CC) -m32 -nostdlib -shared -Wl,--hash-style=sysv -Wl,-soname,libc.so -o $@ lib/tlibc.o

programs/hello_dyn.elf: userland/tests/hello_dyn.c userland/tests/dyn_start.s lib/libc.so
	$(CC) -m32 -c userland/tests/dyn_start.s -o programs/crt0_dyn.o
	$(CC) -m32 -nostdlib -fno-stack-protector -fno-pic -c userland/tests/hello_dyn.c -o programs/hello_dyn.o
	$(CC) -m32 -nostdlib -Wl,-Ttext-segment=0x400000 -Wl,-e,_start -Wl,--dynamic-linker=/lib/libc.so \
	      -o $@ programs/crt0_dyn.o programs/hello_dyn.o -Llib -l:libc.so

initrd.tar: $(PROGRAMS)
	tar cf $@ --format=ustar $^

tOS.iso: kernel/tOS.elf initrd.tar
	mkdir -p iso/boot/grub
	cp kernel/tOS.elf iso/boot/
	cp initrd.tar iso/boot/
	cp boot/grub/grub.cfg iso/boot/grub/
	PATH="/tmp/xorriso_ubuntu/usr/bin:$$PATH" \
	LD_LIBRARY_PATH="/tmp/xorriso_ubuntu/usr/lib/x86_64-linux-gnu:$$LD_LIBRARY_PATH" \
	grub-mkrescue -o $@ iso

run: tOS.iso
	qemu-system-x86_64 -cdrom tOS.iso -m 256M \
		-drive file=/tmp/tfs_test.img,if=ide,format=raw \
		-serial stdio 2>/dev/null || \
	qemu-system-x86_64 -cdrom tOS.iso -m 256M \
		-drive file=/tmp/tfs_test.img,if=ide,format=raw

run-audio: tOS.iso
	qemu-system-x86_64 -cdrom tOS.iso -m 256M \
		-drive file=/tmp/tfs_test.img,if=ide,format=raw \
		-soundhw sb16 -serial stdio 2>/dev/null || \
	qemu-system-x86_64 -cdrom tOS.iso -m 256M \
		-drive file=/tmp/tfs_test.img,if=ide,format=raw \
		-soundhw sb16

run-noinstall: tOS.iso
	rm -f /tmp/tfs_test.img
	dd if=/dev/zero of=/tmp/tfs_test.img bs=1M count=64 >/dev/null 2>&1
	qemu-system-x86_64 -cdrom tOS.iso -m 256M \
		-drive file=/tmp/tfs_test.img,if=ide,format=raw \
		-serial stdio 2>/dev/null || true

clean:
	rm -f $(KERNEL_OBJS) kernel/tOS.elf initrd.tar tOS.iso programs/hello.elf
	rm -f lib/libc.so lib/tlibc.o programs/hello_dyn.elf programs/hello_dyn.o programs/crt0_dyn.o
	rm -rf iso
