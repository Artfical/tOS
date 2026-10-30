/* The shell sshd starts for every account: BusyBox's ash (the busybox package), keeping argv[0] so that
 * "-sh" is a login shell and "sh -c command" runs one command. */
#include <unistd.h>
int main(int argc, char **argv)
{
    (void)argc;
    argv[0] = argv[0][0] == '-' ? "-sh" : "sh";
    execv("/system/pkg/busybox/bin/busybox", argv);
    return 127;
}
