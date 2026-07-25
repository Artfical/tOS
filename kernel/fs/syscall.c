#include "syscall.h"
#include "isr.h"
#include "terminal.h"
#include "fs.h"
#include "ramfs.h"
#include "memory.h"
#include "paging.h"
#include "usermode.h"
#include "string.h"
#include "dns.h"
#include "tcp.h"
#include "bochs.h"
#include "keyboard.h"

#define TOS_O_WRONLY 0x0001
#define TOS_O_RDWR   0x0002
#define TOS_O_CREAT  0x0040
#define TOS_O_TRUNC  0x0200

static bochs_device_t gfx_dev;
static int gfx_ready = 0;

#define FD_MAX 64
#define FD_FREE 0
#define FD_FILE 1
#define FD_CONSOLE 2

typedef struct {
    int type;
    fs_file_t file;
} fd_entry_t;

static fd_entry_t fd_table[FD_MAX];
static uint32_t program_break = 0x800000;

static int fd_alloc(void)
{
    for (int i = 3; i < FD_MAX; i++) {
        if (fd_table[i].type == FD_FREE) return i;
    }
    return -1;
}

static void syscall_stub(registers_t *regs)
{
    uint32_t result = syscall_handler(regs->eax, regs->ebx, regs->ecx, regs->edx, regs->esi);
    regs->eax = result;
}

uint32_t syscall_handler(uint32_t syscall, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    (void)d;

    switch (syscall) {
        case SYS_EXIT:
            sys_exit_longjmp();

        case SYS_FORK:
            return -1;

        case SYS_READ: {
            int fd = (int)a;
            char *buf = (char *)b;
            int count = (int)c;
            if (fd == 0) {
                int i;
                for (i = 0; i < count; i++) {
                    char ch = keyboard_getchar();
                    terminal_putchar(ch);
                    buf[i] = ch;
                    if (ch == '\n') { i++; break; }
                }
                return i;
            }
            if (fd >= 0 && fd < FD_MAX && fd_table[fd].type == FD_FILE) {
                /* ramfs_read()/ramfs_write() return a byte count on
                 * success (or negative on failure), not a 0/-1 status
                 * -- checking `== 0` here always failed (to_read is
                 * essentially never exactly 0), so file reads through
                 * this syscall never actually worked before. */
                fs_file_t *f = &fd_table[fd].file;
                uint32_t to_read = count;
                if (f->offset + to_read > f->size)
                    to_read = f->size - f->offset;
                int n = ramfs_read(f->name, buf, to_read, f->offset);
                if (n < 0) return -1;
                f->offset += n;
                return n;
            }
            return -1;
        }

        case SYS_WRITE: {
            int fd = (int)a;
            const char *buf = (const char *)b;
            int count = (int)c;
            if (fd == 1 || fd == 2) {
                for (int i = 0; i < count; i++) {
                    if (buf[i] == '\n')
                        terminal_putchar('\n');
                    else
                        terminal_putchar(buf[i]);
                }
                return count;
            }
            if (fd >= 0 && fd < FD_MAX && fd_table[fd].type == FD_FILE) {
                fs_file_t *f = &fd_table[fd].file;
                int n = ramfs_write(f->name, buf, count, f->offset);
                if (n < 0) return -1;
                f->offset += n;
                if (f->offset > f->size) f->size = f->offset;
                return n;
            }
            return -1;
        }

        case SYS_OPEN: {
            /* Deliberately does NOT go through fs_open()/fs.c -- that's
             * a separate, never-initialized static file table
             * (fs_init() is never called anywhere in the boot path)
             * left over from before ramfs existed, always empty, so
             * SYS_OPEN silently failed for every path until now. ramfs
             * is the filesystem everything else in tOS actually uses. */
            const char *path = (const char *)a;
            int flags = (int)b;
            if (!ramfs_exists(path)) {
                if (!(flags & TOS_O_CREAT)) return -1;
                if (ramfs_create(path) != 0) return -1;
            }
            int fd = fd_alloc();
            if (fd < 0) return -1;
            fs_file_t *f = &fd_table[fd].file;
            int i = 0;
            while (path[i] && i < FS_NAME_LEN - 1) { f->name[i] = path[i]; i++; }
            f->name[i] = 0;
            f->size = ramfs_size(path);
            f->offset = 0;
            f->exists = 1;
            fd_table[fd].type = FD_FILE;
            return fd;
        }

        case SYS_CLOSE: {
            int fd = (int)a;
            if (fd >= 0 && fd < FD_MAX) {
                fd_table[fd].type = FD_FREE;
                return 0;
            }
            return -1;
        }

        case SYS_WAITPID:
            return -1;

        /* ELF loading/exec support has been removed: the loader wrote
         * PT_LOAD segment data straight to phdr->p_vaddr with no
         * bounds check, and this kernel's paging_init() maps *all*
         * physical RAM (including kernel memory) with PTE_USER from
         * boot -- so any user-supplied binary could point a segment
         * at kernel memory and overwrite it via a plain memcpy(), a
         * straightforward privilege-escalation primitive. Fixing that
         * properly needs real per-process address space isolation
         * (kernel pages not user-accessible, non-identity per-process
         * mappings), which is a much larger change than a point fix,
         * so exec is disabled rather than shipped half-fixed. */
        case SYS_EXECVE:
            (void)a;
            return -1;

        case SYS_CHDIR:
            return 0;

        case SYS_BRK: {
            uint32_t addr = a;
            if (addr == 0)
                return program_break;
            if (addr < 0x800000)
                addr = 0x800000;
            uint32_t old = program_break;
            for (uint32_t p = old; p < addr; p += 0x1000) {
                if (!paging_virt_to_phys(NULL, p)) {
                    uint32_t phys = alloc_physical_page();
                    if (!phys) return -1;
                    paging_map(p, phys, PTE_USER | PTE_WRITABLE);
                }
            }
            program_break = addr;
            return old;
        }

        case SYS_LSEEK: {
            int fd = (int)a;
            int offset = (int)b;
            int whence = (int)c;
            if (fd >= 0 && fd < FD_MAX && fd_table[fd].type == FD_FILE) {
                fs_file_t *f = &fd_table[fd].file;
                uint32_t new_off;
                if (whence == 0) new_off = offset;
                else if (whence == 1) new_off = f->offset + offset;
                else if (whence == 2) new_off = f->size + offset;
                else return -1;
                if (new_off > f->size) new_off = f->size;
                f->offset = new_off;
                return new_off;
            }
            return -1;
        }

        case SYS_GETPID:
            return 1;

        case SYS_KILL:
            if (b == 15 || b == 9) {
                sys_exit_longjmp();
            }
            return -1;

        case SYS_ISATTY: {
            int fd = (int)a;
            if (fd >= 0 && fd <= 2) return 1;
            return 0;
        }

        case SYS_FSTAT: {
            int fd = (int)a;
            struct tos_stat *st = (struct tos_stat *)b;
            if (!st) return -1;
            memset(st, 0, sizeof(*st));
            if (fd >= 0 && fd <= 2) {
                st->st_mode = 0x2000;
                return 0;
            }
            if (fd >= 0 && fd < FD_MAX && fd_table[fd].type == FD_FILE) {
                st->st_mode = 0x8000;
                st->st_size = fd_table[fd].file.size;
                st->st_blksize = 512;
                st->st_blocks = (st->st_size + 511) / 512;
                return 0;
            }
            return -1;
        }

        case SYS_NET_RESOLVE: {
            const char *host = (const char *)a;
            uint32_t ip = 0;
            if (dns_resolve(host, &ip) != 0) return 0;
            return ip;
        }

        case SYS_NET_CONNECT: {
            uint32_t ip = a;
            uint16_t port = (uint16_t)b;
            return tcp_connect(ip, port);
        }

        case SYS_NET_SEND: {
            void *data = (void *)a;
            int len = (int)b;
            return tcp_send(data, len);
        }

        case SYS_NET_RECV: {
            uint8_t *buf = (uint8_t *)a;
            int max_len = (int)b;
            return tcp_recv(buf, max_len);
        }

        case SYS_NET_CLOSE:
            tcp_close();
            return 0;

        case SYS_GFX_INIT: {
            int width = (int)a;
            int height = (int)b;
            if (bochs_init(&gfx_dev) != 0) return -1;
            if (bochs_set_mode(&gfx_dev, width, height, 32) != 0) return -1;
            /* The LFB is a PCI BAR address, not RAM -- it sits well
             * above paging_init()'s identity-mapped [0, total_mem)
             * range and was never actually paged in, so bochs_put_pixel
             * dereferencing dev->lfb directly would fault. Map it here
             * (identity: virt == phys, matching what bochs_put_pixel
             * assumes) before anything writes through it. */
            uint32_t fb_bytes = (uint32_t)width * (uint32_t)height * 4;
            uint32_t fb_pages = (fb_bytes + 4095) / 4096;
            for (uint32_t i = 0; i < fb_pages; i++) {
                uint32_t addr = gfx_dev.lfb + i * 4096;
                paging_map(addr, addr, PTE_PRESENT | PTE_WRITABLE);
            }
            gfx_ready = 1;
            return 0;
        }

        case SYS_GFX_PUTPIXEL: {
            if (!gfx_ready) return -1;
            int x = (int)a;
            int y = (int)b;
            uint32_t color = c;
            bochs_put_pixel(&gfx_dev, x, y, color);
            return 0;
        }

        default:
            return -1;
    }
}

void syscall_init(void)
{
    for (int i = 0; i < FD_MAX; i++)
        fd_table[i].type = FD_FREE;
    fd_table[0].type = FD_CONSOLE;
    fd_table[1].type = FD_CONSOLE;
    fd_table[2].type = FD_CONSOLE;
    isr_register_handler(0x80, syscall_stub);
}
