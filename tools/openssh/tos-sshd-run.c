/* What the sshd service runs: makes the host key the first time, then becomes sshd (kept in the foreground
 * for tOS's service manager). */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define KEY "/etc/ssh/ssh_host_ed25519_key"
int main(void)
{
    struct stat st;
    mkdir("/etc", 0755);
    mkdir("/etc/ssh", 0755);
    if (stat(KEY, &st) != 0) {
        puts("sshd: making the host key");
        fflush(stdout);
        pid_t p = fork();
        if (p == 0) {
            execl("/system/pkg/openssh/bin/ssh-keygen", "ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", KEY, (char *)0);
            _exit(127);
        }
        int status = 0;
        waitpid(p, &status, 0);
        if (stat(KEY, &st) != 0) { puts("sshd: could not make the host key"); return 1; }
        chmod(KEY, 0600);
    }
    execl("/system/pkg/openssh/sbin/sshd", "/system/pkg/openssh/sbin/sshd", "-D", "-e", "-f", "/system/pkg/openssh/etc/sshd_config", (char *)0);
    puts("sshd: could not start /system/pkg/openssh/sbin/sshd");
    return 127;
}
