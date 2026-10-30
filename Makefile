CC = gcc
CXX = g++
AS = as
LD = ld
AR = ar

# tOS is a 64-bit kernel: the top 2 GB (-mcmodel=kernel), no red zone (interrupts use the stack).
# SSE is on: every interrupt entry saves the interrupted FPU/SSE state (isr_stubs.s), and the
# scheduler, which switches it, is built to use none itself.
ARCH_CFLAGS = -m64 -mcmodel=kernel -mno-red-zone
GCC_INC := $(shell $(CC) -m64 -print-file-name=include)
HOST_MULTIARCH := $(shell $(CC) -print-multiarch)
HOST_MULTIARCH_INCLUDE := $(wildcard /usr/include/$(HOST_MULTIARCH))

# Regular kernel flags (no nostdinc to avoid breaking existing code)
CFLAGS = -MMD -MP $(ARCH_CFLAGS) -ffreestanding -nostdlib -nostartfiles -nodefaultlibs \
         -fno-builtin -fno-stack-protector -fno-pic -fno-pie -fno-strict-aliasing \
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

ifeq ($(FPU_CONTEXT_SELFTEST),1)
CFLAGS += -DTOS_FPU_CONTEXT_SELFTEST
kernel/core/kernel.o kernel/core/scheduler.o: FORCE
endif
ifeq ($(ELF32_SELFTEST),1)
CFLAGS += -DTOS_ELF32_SELFTEST
kernel/core/kernel.o: FORCE
initrd.tar: FORCE
endif
ifeq ($(NODE_STATIC_SELFTEST),1)
CFLAGS += -DTOS_NODE_STATIC_SELFTEST
kernel/core/kernel.o: FORCE
endif

# Relaxed flags for MicroPython core. It includes a few host libc headers;
# use the installed host multiarch headers rather than a machine-specific
# /tmp symlink. With GCC -m32, -print-multiarch names the target tuple, which
# may not be the directory containing the host's shared libc headers.
MPY_HOST_INCLUDE = $(if $(HOST_MULTIARCH_INCLUDE),-isystem $(HOST_MULTIARCH_INCLUDE))
MPY_CFLAGS = -MMD -MP $(ARCH_CFLAGS) -ffreestanding -nostdlib -nostartfiles -nodefaultlibs \
             -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
             -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
             -O2 -Wall -Wno-unused-variable -Wno-unused-function \
             -Wno-unused-parameter -Wno-sign-compare \
             -Wno-missing-field-initializers -Wno-implicit-fallthrough \
             $(MPY_HOST_INCLUDE) \
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

# 1990s vendored C (id Software/doomgeneric) trips plenty of things
# -Wall/-Wextra/-Werror rightly reject in tOS's own code, same reason
# MicroPython gets its own relaxed MPY_CFLAGS above. -Ikernel/doom/port
# comes first so <string.h>/<stdlib.h>/<math.h>/etc resolve to that
# directory's wrapper headers (which #include_next through to the real
# kernel/lib ones, adding only the handful of extra declarations DOOM
# needs -- see kernel/doom/port/doom_compat.c) instead of kernel/lib's
# directly.
DOOM_CFLAGS = -MMD -MP $(ARCH_CFLAGS) -ffreestanding -nostdlib -nostartfiles -nodefaultlibs \
             -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
             -O2 -Wall -Wno-unused-variable -Wno-unused-function \
             -Wno-unused-parameter -Wno-sign-compare -Wno-unused-but-set-variable \
             -Wno-missing-field-initializers -Wno-implicit-fallthrough \
             -Wno-missing-braces -Wno-parentheses -Wno-format \
             -Wno-type-limits -Wno-unused-value \
             -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DFEATURE_SOUND \
             -Ikernel/doom/port -Ikernel/doom \
             -I. -Ikernel/core -Ikernel/display \
             -Ikernel/fs -Ikernel/shell -Ikernel/shell/commands -Ikernel/lib -Ikernel/net \
             -Ikernel/drivers/include -Ikernel/drivers/bus -Ikernel/drivers/storage \
             -Ikernel/drivers/net -Ikernel/drivers/usb -Ikernel/drivers/audio \
             -Ikernel/drivers/video -Ikernel/drivers/input -Ikernel/drivers/system \
             -Ikernel/drivers/misc -Ikernel/audio

LDFLAGS = -m elf_x86_64 -z max-page-size=4096 -T kernel/boot/linker64.ld
ASFLAGS = --64

# Vendored C++ (Wolf4SDL, itself a port of id Software's original
# Wolfenstein 3D) -- unlike doomgeneric, this engine calls SDL2
# directly from within its own source rather than through a small
# platform-specific shim, so kernel/wolf3d/port/ provides SDL.h/
# SDL_mixer.h/SDL_syswm.h compatibility headers backed by tOS's own
# framebuffer/keyboard/mouse/audio code instead of a real SDL2. This
# is a from-scratch freestanding C++ setup (no libstdc++, no
# exceptions/RTTI/threads -- tOS has no thread-safe statics or stack
# unwinding support), same general idea as DOOM_CFLAGS below but with
# g++-specific flags added.
WOLF_CXXFLAGS = -MMD -MP $(ARCH_CFLAGS) -ffreestanding -nostdlib -nostartfiles -nodefaultlibs \
             -fno-exceptions -fno-rtti -fno-use-cxa-atexit \
             -fno-threadsafe-statics -fno-stack-protector -fno-pic -fno-pie \
             -O2 -Wall -Wno-unused-variable -Wno-unused-function \
             -Wno-unused-parameter -Wno-sign-compare -Wno-unused-but-set-variable \
             -Wno-missing-field-initializers -Wno-implicit-fallthrough \
             -Wno-missing-braces -Wno-parentheses -Wno-format \
             -Wno-type-limits -Wno-unused-value -Wno-reorder \
             -Ikernel/wolf3d/port -Ikernel/wolf3d \
             -I. -Ikernel/core -Ikernel/display \
             -Ikernel/fs -Ikernel/shell -Ikernel/shell/commands -Ikernel/lib -Ikernel/net \
             -Ikernel/drivers/include -Ikernel/drivers/bus -Ikernel/drivers/storage \
             -Ikernel/drivers/net -Ikernel/drivers/usb -Ikernel/drivers/audio \
             -Ikernel/drivers/video -Ikernel/drivers/input -Ikernel/drivers/system \
             -Ikernel/drivers/misc -Ikernel/audio

WOLF_CFLAGS = -MMD -MP $(ARCH_CFLAGS) -ffreestanding -nostdlib -nostartfiles -nodefaultlibs \
             -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
             -O2 -Wall -Wno-unused-variable -Wno-unused-function \
             -Wno-unused-parameter -Wno-sign-compare -Wno-unused-but-set-variable \
             -Wno-missing-field-initializers -Wno-implicit-fallthrough \
             -Wno-missing-braces -Wno-parentheses -Wno-format \
             -Wno-type-limits -Wno-unused-value \
             -Ikernel/wolf3d/port -Ikernel/wolf3d \
             -I. -Ikernel/core -Ikernel/display \
             -Ikernel/fs -Ikernel/shell -Ikernel/shell/commands -Ikernel/lib -Ikernel/net \
             -Ikernel/drivers/include -Ikernel/drivers/bus -Ikernel/drivers/storage \
             -Ikernel/drivers/net -Ikernel/drivers/usb -Ikernel/drivers/audio \
             -Ikernel/drivers/video -Ikernel/drivers/input -Ikernel/drivers/system \
             -Ikernel/drivers/misc -Ikernel/audio

# Auto-discover driver sources
DRIVER_SRCS := $(wildcard kernel/drivers/bus/*.c kernel/drivers/storage/*.c \
    kernel/drivers/net/*.c kernel/drivers/usb/*.c kernel/drivers/audio/*.c \
    kernel/drivers/video/*.c kernel/drivers/input/*.c kernel/drivers/system/*.c \
    kernel/drivers/misc/*.c)
DRIVER_OBJS := $(DRIVER_SRCS:.c=.o)

KERNEL_OBJS = \
    kernel/boot/boot64.o \
    kernel/boot/isr_stubs.o \
    kernel/core/kernel.o \
    kernel/display/terminal.o \
    kernel/display/vt.o \
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
    kernel/lib/udivti3.o \
    kernel/fs/fs.o \
    kernel/fs/vfs.o \
    kernel/fs/volumes.o \
    kernel/fs/fsbridge.o \
    kernel/fs/syscall.o \
    kernel/fs/linux64.o \
    kernel/fs/tty.o \
    kernel/core/service.o \
    kernel/core/serial.o \
    kernel/core/debugmon.o \
    kernel/core/sound.o \
    kernel/core/scheduler.o \
    kernel/core/auth.o \
    kernel/core/paging.o \
    kernel/core/tss.o \
    kernel/core/usermode.o \
    kernel/core/proc.o \
    kernel/core/elf32.o \
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
    kernel/shell/commands/cmd_user.o \
    kernel/shell/commands/cmd_tpkg.o \
    kernel/shell/commands/cmd_service.o \
    kernel/core/klog.o \
    kernel/fs/ramfs.o \
    kernel/fs/perm.o \
    kernel/fs/sysdirs.o \
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
    kernel/core/build.o \
    kernel/core/update.o \
    kernel/core/kexec.o \
    kernel/core/kexec_tramp.o \
    kernel/display/wm_gfx.o \
    kernel/display/wm_notify.o \
    kernel/display/bootsplash.o \
    kernel/display/bootlogo.o \
    kernel/display/gfx.o \
    kernel/display/ui_assets.o \
    kernel/display/uiapp.o \
    kernel/display/uifiles.o \
    kernel/display/uihost_tos.o \
    kernel/display/apps/calc.o \
    kernel/display/apps/diskutil.o \
    kernel/display/apps/recovery.o \
    kernel/display/apps/updatenotes.o \
    kernel/display/apps/clock.o \
    kernel/display/apps/g2048.o \
    kernel/display/apps/notepad2.o \
    kernel/display/apps/files.o \
    kernel/display/apps/viewer2.o \
    kernel/display/apps/activity.o \
    kernel/display/apps/network.o \
    kernel/display/apps/paint2.o \
    kernel/display/apps/notes.o \
    kernel/display/apps/tfire.o \
    kernel/display/apps/tfire_notices.o \
    kernel/display/apps/settings.o \
    kernel/display/imgload.o \
    kernel/display/jpeg.o \
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
    kernel/display/fbconsole.o \
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
    kernel/net/dhcp.o \
    kernel/net/tcp.o \
    kernel/net/lsock.o \
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
    kernel/net/sha1.o \
    kernel/net/websocket.o \
    kernel/net/sha512.o \
    kernel/net/rsa.o \
    kernel/net/x509.o \
    kernel/net/ca_store.o \
    kernel/net/csprng.o \
    kernel/net/aes.o \
    kernel/net/gcm.o \
    kernel/net/x25519.o \
    kernel/net/p256.o \
    kernel/net/bignum.o \
    kernel/net/tls.o \
    kernel/net/https.o \
    kernel/net/webfetch.o \
    kernel/net/http.o \
    kernel/audio/audio.o \
    kernel/audio/wav_decoder.o \
    kernel/audio/mp3_decoder.o \
    kernel/audio/aac_decoder.o \
    kernel/audio/m4a_demux.o \
        kernel/audio/demo_song_embed.o \
    kernel/display/mediaplayer.o \
    kernel/display/netmon.o \
    kernel/display/snake.o \
    kernel/display/game2048.o \
    kernel/display/pdf_parse.o \
    kernel/display/pdfview.o \
    kernel/display/stickynotes.o \
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
    kernel/micropython/ports/tos/modgitcrypto.o \
    kernel/micropython/ports/tos/runfile.o \
    kernel/micropython/shared/runtime/pyexec.o \
    kernel/micropython/shared/readline/readline.o

KERNEL_OBJS += $(DRIVER_OBJS)
KERNEL_OBJS += kernel/tfire/tfire_all.o

# MicroPython py/ core sources (exclude native/asm/persistent)
MPY_PY_SRCS := $(filter-out %/asmarm.c %/asmthumb.c %/asmxtensa.c %/asmrv32.c \
    %/emitnative.c %/emitinlinethumb.c %/emitinlinextensa.c %/emitinlinerrv32.c \
    %/persistentcode.c, $(wildcard kernel/micropython/py/*.c))
MPY_PY_OBJS := $(MPY_PY_SRCS:.c=.o)
KERNEL_OBJS += $(MPY_PY_OBJS)

# Vendored id Software/doomgeneric source (kernel/doom/LICENSE, GPLv2)
# plus tOS's own platform glue (kernel/doom/port/) -- see README's
# "Linear framebuffer graphics" section.
DOOM_SRCS := $(wildcard kernel/doom/*.c) $(wildcard kernel/doom/port/*.c)
DOOM_OBJS := $(DOOM_SRCS:.c=.o)
KERNEL_OBJS += $(DOOM_OBJS)

# Vendored Wolf4SDL source (kernel/wolf3d/LICENSE, GPLv2) -- itself a
# port of id Software's original Wolfenstein 3D to SDL2 -- plus tOS's
# own SDL2 compatibility shim (kernel/wolf3d/port/).
WOLF_CXX_SRCS := $(wildcard kernel/wolf3d/*.cpp) $(wildcard kernel/wolf3d/port/*.cpp)
WOLF_C_SRCS := $(wildcard kernel/wolf3d/*.c) $(wildcard kernel/wolf3d/port/*.c)
WOLF_ASM_SRCS := $(wildcard kernel/wolf3d/port/*.s)
WOLF_OBJS := $(WOLF_CXX_SRCS:.cpp=.o) $(WOLF_C_SRCS:.c=.o) $(WOLF_ASM_SRCS:.s=.o)
KERNEL_OBJS += $(WOLF_OBJS)

PROGRAMS = assets/boot/bootvideo.bin programs/tosgui_demo.py programs/hello.t programs/bsstest.t \
	programs/filetest.t programs/gfxtest.t programs/termtest.t programs/nettest.t programs/cryptotest.t assets/doom1.wad \
	assets/wolf3d/audiohed.wl1 assets/wolf3d/audiot.wl1 assets/wolf3d/gamemaps.wl1 \
	assets/wolf3d/maphead.wl1 assets/wolf3d/vgadict.wl1 assets/wolf3d/vgagraph.wl1 \
	assets/wolf3d/vgahead.wl1 assets/wolf3d/vswap.wl1

ifeq ($(ELF32_SELFTEST),1)
PROGRAMS += programs/elf32_hello.elf
endif

.PHONY: all clean run iso

all: tOS.iso

# MicroPython py/ core files use relaxed flags
kernel/micropython/py/%.o: kernel/micropython/py/%.c
	$(CC) $(MPY_CFLAGS) -c $< -o $@

kernel/micropython/shared/%.o: kernel/micropython/shared/%.c
	$(CC) $(MPY_CFLAGS) -c $< -o $@

kernel/micropython/ports/tos/%.o: kernel/micropython/ports/tos/%.c
	$(CC) $(MPY_PORT_CFLAGS) -c $< -o $@

kernel/doom/port/%.o: kernel/doom/port/%.c
	$(CC) $(DOOM_CFLAGS) -c $< -o $@

kernel/doom/%.o: kernel/doom/%.c
	$(CC) $(DOOM_CFLAGS) -c $< -o $@

# Wolf4SDL and DOOM are two entirely separate id Software engines from
# different eras, each with its own small, generic, non-static global
# variable names (view*, states, gamestate, configdir, MainMenu, ...)
# that happen to collide once both are linked into the same
# kernel/tOS.elf -- C++ doesn't mangle plain data symbols the way it
# mangles function names, so `int viewx;` in kernel/wolf3d/wl_draw.cpp
# and `int viewx;` in kernel/doom/r_main.c are, as far as the linker
# is concerned, the exact same symbol. objcopy --redefine-sym renames
# just Wolf3D's copies with a w3d_ prefix after each object file
# compiles (a no-op for any object that doesn't happen to define or
# reference a given name, so this list is safe to apply uniformly
# across every kernel/wolf3d/ object). Found by compiling the whole
# tree and reading every "multiple definition" error the linker
# reported -- if a future source change introduces a new colliding
# global, the same error will point at exactly which name to add here.
WOLF_REDEFINE_SYMS = --redefine-sym rndindex=w3d_rndindex \
	--redefine-sym states=w3d_states \
	--redefine-sym finetangent=w3d_finetangent \
	--redefine-sym viewcos=w3d_viewcos \
	--redefine-sym viewsin=w3d_viewsin \
	--redefine-sym viewangle=w3d_viewangle \
	--redefine-sym viewy=w3d_viewy \
	--redefine-sym viewx=w3d_viewx \
	--redefine-sym gamestate=w3d_gamestate \
	--redefine-sym demoname=w3d_demoname \
	--redefine-sym centerx=w3d_centerx \
	--redefine-sym viewheight=w3d_viewheight \
	--redefine-sym viewwidth=w3d_viewwidth \
	--redefine-sym configdir=w3d_configdir \
	--redefine-sym menuactive=w3d_menuactive \
	--redefine-sym MainMenu=w3d_MainMenu \
	--redefine-sym demobuffer=w3d_demobuffer \
	--redefine-sym demoplayback=w3d_demoplayback

kernel/wolf3d/port/%.o: kernel/wolf3d/port/%.c
	$(CC) $(WOLF_CFLAGS) -c $< -o $@
	objcopy $(WOLF_REDEFINE_SYMS) $@

kernel/wolf3d/%.o: kernel/wolf3d/%.c
	$(CC) $(WOLF_CFLAGS) -c $< -o $@
	objcopy $(WOLF_REDEFINE_SYMS) $@

kernel/wolf3d/%.o: kernel/wolf3d/%.cpp
	$(CXX) $(WOLF_CXXFLAGS) -c $< -o $@
	objcopy $(WOLF_REDEFINE_SYMS) $@


# tFire's engine (kernel/tfire) and what it is built on (third_party: NetSurf's libparserutils, libwapcaplet,
# libhubbub and libcss; QuickJS; musl's libm; compiler-rt's floating point). All of it is built for software
# floating point (-mno-80387: the kernel never saves the FPU), against tFire's own small C library
# (kernel/tfire/libc), and linked into one object whose only global symbols are the engine's API (TFIRE_API):
# the C library functions it brings along stay its own and cannot clash with DOOM's or Wolf3D's.
TFIRE_BASE = -MMD -MP $(ARCH_CFLAGS) -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
             -O2 -std=gnu99 \
             -U__linux__ -U__linux -Ulinux -U__unix__ -U__unix -Uunix -DTOS_TFIRE \
             -DWITHOUT_ICONV_FILTER -DNDEBUG -DCONFIG_VERSION=\"2025-09-13\" -Ikernel/tfire/libc -Ikernel/lib
NS = third_party/netsurf
# Newer compilers (GCC 14 and on) make some old C an error; third-party code still writes it, and -w is meant to
# keep it quiet, so those are warnings again there
TFIRE_TP_OLDC = -Wno-error=implicit-function-declaration -Wno-error=implicit-int -Wno-error=int-conversion \
                -Wno-error=incompatible-pointer-types
TFIRE_NS_INC = -I$(NS)/libparserutils/include -I$(NS)/libwapcaplet/include -I$(NS)/libhubbub/include -I$(NS)/libcss/include
TFIRE_CFLAGS = $(TFIRE_BASE) -Wall -Wextra -Werror -Wno-unused-parameter $(TFIRE_NS_INC) -Ithird_party/quickjs \
             -Ikernel/tfire -Ithird_party/nanosvg -Ithird_party/libwebp -Ithird_party/wasm3/source -Dd_m3Use32BitSlots=0 -Ikernel/core -Ikernel/display -Ikernel/net -Ikernel/fs -Ikernel/drivers/include

TFIRE_NS_SRCS := $(shell find $(NS) -name '*.c' | grep -v 'css_property_parser_gen\|/test/\|autogenerated-element-type.c')
TFIRE_TP_SRCS := $(TFIRE_NS_SRCS) \
    $(addprefix third_party/quickjs/,quickjs.c libregexp.c libunicode.c cutils.c dtoa.c) \
    $(wildcard third_party/musl-libm/src/*.c) $(wildcard third_party/compiler-rt/*.c) \
    $(wildcard third_party/libwebp/src/dec/*.c) $(wildcard third_party/libwebp/src/utils/*.c) $(wildcard third_party/libwebp/src/dsp/*.c) \
    $(wildcard third_party/wasm3/source/*.c)
TFIRE_OWN_SRCS := $(wildcard kernel/tfire/*.c) $(wildcard kernel/tfire/libc/*.c)
TFIRE_PARTS := $(TFIRE_TP_SRCS:.c=.o) $(TFIRE_OWN_SRCS:.c=.o) kernel/tfire/js_prelude.o
TFIRE_API = page_open page_open_html page_close page_tick page_loading page_fetched page_take_download page_url page_title page_layout page_picture \
    page_mask page_focus page_caret_on page_click page_key page_scrolled page_take_scroll page_take_navigation \
    page_take_url_change page_resolve page_hover page_link_at page_frame page_scroll_y dom_attr tf_free tf_malloc tf_alloc_usage

$(NS)/libparserutils/%.o: $(NS)/libparserutils/%.c
	$(CC) $(TFIRE_BASE) -w $(TFIRE_TP_OLDC) -I$(NS)/libparserutils/include -I$(NS)/libparserutils/src -c $< -o $@
$(NS)/libwapcaplet/%.o: $(NS)/libwapcaplet/%.c
	$(CC) $(TFIRE_BASE) -w $(TFIRE_TP_OLDC) -I$(NS)/libwapcaplet/include -c $< -o $@
$(NS)/libhubbub/%.o: $(NS)/libhubbub/%.c
	$(CC) $(TFIRE_BASE) -w $(TFIRE_TP_OLDC) -I$(NS)/libhubbub/include -I$(NS)/libhubbub/src -I$(NS)/libparserutils/include -c $< -o $@
$(NS)/libcss/%.o: $(NS)/libcss/%.c
	$(CC) $(TFIRE_BASE) -w $(TFIRE_TP_OLDC) -I$(NS)/libcss/include -I$(NS)/libcss/src -I$(NS)/libparserutils/include -I$(NS)/libwapcaplet/include -c $< -o $@
third_party/quickjs/%.o: third_party/quickjs/%.c
	$(CC) $(TFIRE_BASE) -w $(TFIRE_TP_OLDC) -Ithird_party/quickjs -c $< -o $@
third_party/musl-libm/%.o: third_party/musl-libm/%.c
	$(CC) $(TFIRE_BASE) -w $(TFIRE_TP_OLDC) -Dhidden= -Ithird_party/musl-libm/src -c $< -o $@
third_party/libwebp/%.o: third_party/libwebp/%.c
	$(CC) $(TFIRE_BASE) -w $(TFIRE_TP_OLDC) -Ithird_party/libwebp -c $< -o $@
third_party/wasm3/%.o: third_party/wasm3/%.c
	$(CC) $(TFIRE_BASE) -w $(TFIRE_TP_OLDC) -Dd_m3Use32BitSlots=0 -Ithird_party/wasm3/source -c $< -o $@
third_party/compiler-rt/%.o: third_party/compiler-rt/%.c
	$(CC) $(TFIRE_BASE) -w $(TFIRE_TP_OLDC) -Ithird_party/compiler-rt -c $< -o $@
kernel/tfire/%.o: kernel/tfire/%.c
	$(CC) $(TFIRE_CFLAGS) -c $< -o $@
kernel/tfire/libc/%.o: kernel/tfire/libc/%.c
	$(CC) $(TFIRE_CFLAGS) -c $< -o $@
kernel/tfire/js_prelude.o: kernel/tfire/js_prelude.s kernel/tfire/prelude.js
	$(AS) $(ASFLAGS) $< -o $@
kernel/tfire/tfire_all.o: $(TFIRE_PARTS)
	$(LD) -m elf_x86_64 -r -o $@.tmp $(TFIRE_PARTS)
	objcopy $(addprefix --keep-global-symbol=,$(TFIRE_API)) $@.tmp $@
	rm -f $@.tmp
# the licences of everything tOS is built from (tFire shows them at tfire:licenses)
THIRD_PARTY_NOTICES.md THIRD_PARTY_NOTICES.html: tools/gen_notices.py
	python3 tools/gen_notices.py
kernel/display/apps/tfire_notices.o: kernel/display/apps/tfire_notices.s THIRD_PARTY_NOTICES.html
	$(AS) $(ASFLAGS) $< -o $@
# the window around it is desktop code, but sees the engine's types
kernel/display/apps/tfire.o: kernel/display/apps/tfire.c
	$(CC) $(CFLAGS) $(TFIRE_NS_INC) -Ikernel/tfire -c $< -o $@

kernel/core/scheduler.o kernel/drivers/system/fpu.o: CFLAGS += -mgeneral-regs-only

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

programs/elf32_hello.o: programs/elf32_hello.s
	$(AS) --32 $< -o $@

programs/elf32_hello.elf: programs/elf32_hello.o programs/elf32_hello.ld
	$(LD) -m elf_i386 -T programs/elf32_hello.ld -o $@ programs/elf32_hello.o

# the 64-bit kernel, inside the 32-bit ELF every Multiboot loader (and older tOS's Software Update) reads
kernel/tOS.elf: $(KERNEL_OBJS) tools/elf32wrap.py
	$(LD) $(LDFLAGS) -o kernel/tOS64.elf $(KERNEL_OBJS)
	python3 tools/elf32wrap.py kernel/tOS64.elf $@

# What 64-bit Linux programs (Node.js, opencode...) are linked against, from the build machine:
# the dynamic linker, glibc and the GCC runtime. LICENSES.txt says which packages they are and
# where their sources are (glibc: LGPL-2.1+; libstdc++ and libgcc_s: GPL-3 with the GCC Runtime
# Library Exception).
LINUX_RUNTIME_LIBS = libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 libstdc++.so.6 libgcc_s.so.1 \
                     libresolv.so.2 libnss_dns.so.2 libnss_files.so.2
linux-runtime/.stamp:
	rm -rf linux-runtime && mkdir -p linux-runtime/lib64 linux-runtime/lib/x86_64-linux-gnu
	cp -L /lib64/ld-linux-x86-64.so.2 linux-runtime/lib64/
	for l in $(LINUX_RUNTIME_LIBS); do cp -L /lib/x86_64-linux-gnu/$$l linux-runtime/lib/x86_64-linux-gnu/; done
	python3 tools/runtime_licenses.py linux-runtime/lib/x86_64-linux-gnu/LICENSES.txt
	cp /usr/share/common-licenses/LGPL-2.1 linux-runtime/lib/x86_64-linux-gnu/LICENSE.LGPL-2.1
	cp /usr/share/common-licenses/GPL-3 linux-runtime/lib/x86_64-linux-gnu/LICENSE.GPL-3
	touch $@

initrd.tar: $(PROGRAMS) linux-runtime/.stamp
	tar cf $@ --format=ustar $(PROGRAMS)
	tar rf $@ --format=ustar -C linux-runtime lib64 lib

tOS.iso: kernel/tOS.elf initrd.tar
	mkdir -p iso/boot/grub
	cp kernel/tOS.elf iso/boot/
	cp initrd.tar iso/boot/
	cp boot/grub/grub.cfg iso/boot/grub/
	PATH="/tmp/xorriso_ubuntu/usr/bin:$$PATH" \
	LD_LIBRARY_PATH="/tmp/xorriso_ubuntu/usr/lib/x86_64-linux-gnu:$$LD_LIBRARY_PATH" \
	grub-mkrescue -o $@ iso

run: tOS.iso
	qemu-system-x86_64 -cdrom tOS.iso -m 1024M \
		-drive file=/tmp/tfs_test.img,if=ide,format=raw \
		-serial stdio 2>/dev/null || \
	qemu-system-x86_64 -cdrom tOS.iso -m 1024M \
		-drive file=/tmp/tfs_test.img,if=ide,format=raw

run-audio: tOS.iso
	qemu-system-x86_64 -cdrom tOS.iso -m 1024M \
		-drive file=/tmp/tfs_test.img,if=ide,format=raw \
		-soundhw sb16 -serial stdio 2>/dev/null || \
	qemu-system-x86_64 -cdrom tOS.iso -m 1024M \
		-drive file=/tmp/tfs_test.img,if=ide,format=raw \
		-soundhw sb16

run-noinstall: tOS.iso
	rm -f /tmp/tfs_test.img
	dd if=/dev/zero of=/tmp/tfs_test.img bs=1M count=64 >/dev/null 2>&1
	qemu-system-x86_64 -cdrom tOS.iso -m 1024M \
		-drive file=/tmp/tfs_test.img,if=ide,format=raw \
		-serial stdio 2>/dev/null || true

# A build's number is the count of commits, less BUILD_OFFSET: commits that never became a published build
# (577-581, withdrawn or failed) are not counted, so the numbers people see go on one by one
TOS_BUILD := $(shell expr $$(git rev-list --count HEAD 2>/dev/null || echo 0) - $$(cat BUILD_OFFSET 2>/dev/null || echo 0))
TOS_COMMIT := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)

# the build number and commit, fresh on every build
kernel/core/build.o: FORCE
	$(CC) $(CFLAGS) -DTOS_BUILD=$(TOS_BUILD) -DTOS_COMMIT='"$(TOS_COMMIT)"' -c kernel/core/build.c -o $@
FORCE:
.PHONY: FORCE

clean:
	rm -f $(KERNEL_OBJS) $(TFIRE_PARTS) programs/elf32_hello.o programs/elf32_hello.elf kernel/tOS.elf initrd.tar tOS.iso
	rm -rf linux-runtime
	rm -rf iso
	find kernel -name '*.d' -delete

# Real header dependency tracking (-MMD -MP on every *_CFLAGS above emits
# a .d file per object, listing every header it actually included) --
# replaces the old hand-maintained "$(CORE_C_OBJS): kernel/core/version.h"
# rule, which only ever covered version.h and nothing else. Without this,
# changing a widely-included header (e.g. adding a field to scheduler.h's
# task_t) leaves every .o that wasn't itself edited stale: they keep
# linking against the old struct layout since make has no way to know
# they depend on it, which is exactly how mouse.o/ps2.o went two months
# without rebuilding despite scheduler.h changing today. The "-" prefix
# means make doesn't complain on a clean checkout, before any .d files
# exist yet.
-include $(KERNEL_OBJS:.o=.d)
-include $(TFIRE_PARTS:.o=.d)
