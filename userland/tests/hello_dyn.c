/* Dynamically-linked test program — every function it calls (puts,
 * printf, exit) is imported from libc.so at load time via tOS's ELF
 * dynamic loader (PT_DYNAMIC / DT_NEEDED / PLT+GOT relocations). */

extern int puts(const char *s);
extern int printf(const char *fmt, ...);
extern void exit(int code);

int main(void)
{
    puts("Hello from a dynamically linked tOS binary!");
    printf("2 + 2 = %d\n", 4);
    exit(0);
    return 0;
}
