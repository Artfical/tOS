#include "syscall.h"
#include "isr.h"
#include "terminal.h"
#include "fs.h"
#include "ramfs.h"
#include "elf.h"
#include "memory.h"
#include "paging.h"
#include "usermode.h"
#include "string.h"

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
                for (int i = 0; i < count; i++)
                    buf[i] = 0;
                return count;
            }
            if (fd >= 0 && fd < FD_MAX && fd_table[fd].type == FD_FILE) {
                fs_file_t *f = &fd_table[fd].file;
                uint32_t to_read = count;
                if (f->offset + to_read > f->size)
                    to_read = f->size - f->offset;
                if (ramfs_read(f->name, buf, to_read, f->offset) == 0) {
                    f->offset += to_read;
                    return to_read;
                }
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
                return count;
            }
            return -1;
        }

        case SYS_OPEN: {
            const char *path = (const char *)a;
            int flags = (int)b;
            (void)flags;
            fs_file_t file;
            if (fs_open(path, &file) == 0) {
                int fd = fd_alloc();
                if (fd >= 0) {
                    fd_table[fd].type = FD_FILE;
                    fd_table[fd].file = file;
                    return fd;
                }
            }
            return -1;
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

        case SYS_EXECVE: {
            const char *path = (const char *)a;
            if (!ramfs_exists(path) || ramfs_is_dir(path))
                return -1;
            uint32_t sz = ramfs_size(path);
            void *prog = malloc(sz);
            if (!prog) return -1;
            ramfs_read(path, prog, sz, 0);
            uint32_t entry = 0;
            if (elf_load(prog, &entry) != 0) {
                free(prog);
                return -1;
            }
            free(prog);
            uint32_t save_esp;
            asm volatile("mov %%esp, %0" : "=r"(save_esp));
            sys_exit_set_jmp(save_esp, (uint32_t)&&after_exec);
            enter_user_mode(entry, 0xBFFFF000);
            after_exec:
            return 0;
        }

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
