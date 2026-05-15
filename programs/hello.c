void _start(void)
{
    const char *msg = "Hello from tOS userspace!\n";

    asm volatile(
        "mov $4, %%eax\n"
        "mov $1, %%ebx\n"
        "mov %0, %%ecx\n"
        "mov %1, %%edx\n"
        "int $0x80\n"
        :
        : "r"(msg), "r"(26)
        : "eax", "ebx", "ecx", "edx"
    );

    asm volatile(
        "mov $1, %%eax\n"
        "mov $0, %%ebx\n"
        "int $0x80\n"
        :
        :
        : "eax", "ebx"
    );
}
