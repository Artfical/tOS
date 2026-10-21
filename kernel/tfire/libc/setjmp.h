#ifndef TFIRE_SETJMP_H
#define TFIRE_SETJMP_H
/* setjmp/longjmp for i386: ebx, esi, edi, ebp, esp and the return address (libc_tfire.c) */
typedef unsigned long jmp_buf[6];
int setjmp(jmp_buf env) __attribute__((returns_twice));
void longjmp(jmp_buf env, int val) __attribute__((noreturn));
#define _setjmp setjmp
#define _longjmp longjmp
#endif
