#ifndef WOLF3D_JMP_H
#define WOLF3D_JMP_H
/* Minimal i386 setjmp()/longjmp() -- tOS has no real implementation of
 * either anywhere else (kernel/core/usermode.c's sys_exit_longjmp() is
 * a one-off, unrelated mechanism for a completely different purpose).
 * Needed here specifically because kernel/wolf3d/wl_main.cpp's Quit()
 * calls exit() on its normal "user chose Quit from the menu" path --
 * and tOS's real exit() (kernel/lib/stdlib.c) just spins forever
 * rather than actually unwinding, exactly the same one-way-trip
 * problem DOOM originally had before Ctrl+C support was added there.
 * Since Wolf4SDL's main()/DemoLoop() has no per-frame callback API to
 * hook into the way doomgeneric does (see kernel/doom/port/
 * doomgeneric_tos.c) -- it's one C++ function call that loops forever
 * internally -- longjmp() back out past however deep the call stack
 * happens to be at the time is the only practical way to return
 * control to the shell/window-close, both for a normal in-game Quit
 * and for Ctrl+C.
 *
 * Saves only what the i386 SysV calling convention requires a callee
 * to preserve across a call (ebx, esi, edi, ebp, esp) plus the return
 * address -- enough to unwind the stack safely, nothing more. */
typedef struct { unsigned long regs[6]; } wolf_jmp_buf;

#ifdef __cplusplus
extern "C" {
#endif

int wolf_setjmp(wolf_jmp_buf *env);
void wolf_longjmp(wolf_jmp_buf *env, int val) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif
