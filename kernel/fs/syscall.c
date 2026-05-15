#include "syscall.h"
#include "isr.h"
#include "terminal.h"
#include "fs.h"
#include "memory.h"

static int running_process = 1;

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
            running_process = 0;
            return 0;

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
            }
            return count;
        }

        case SYS_READ: {
            return 0;
        }

        case SYS_OPEN: {
            const char *path = (const char *)a;
            fs_file_t file;
            if (fs_open(path, &file) == 0) {
                fs_file_t *f = (fs_file_t *)malloc(sizeof(fs_file_t));
                if (f) {
                    *f = file;
                    return (uint32_t)f;
                }
            }
            return -1;
        }

        case SYS_CLOSE: {
            free((void *)a);
            return 0;
        }

        case SYS_GETPID:
            return 1;

        case SYS_BRK:
            return 0;

        default:
            return -1;
    }
}

void syscall_init(void)
{
    isr_register_handler(0x80, syscall_stub);
}
