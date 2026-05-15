#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

#define SYS_EXIT 1
#define SYS_WRITE 4
#define SYS_READ 5
#define SYS_OPEN 6
#define SYS_CLOSE 7
#define SYS_GETPID 20
#define SYS_BRK 45

void syscall_init(void);
uint32_t syscall_handler(uint32_t syscall, uint32_t a, uint32_t b, uint32_t c, uint32_t d);

#endif
