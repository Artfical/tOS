/* POSIX semaphores and shared memory (/dev/shm): what multiprocessing and many programs use */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("semtest: FAIL %s (errno %d)\n", what, errno); fails++; } else printf("semtest: ok %s\n", what); } while (0)
int main(void)
{
    struct stat st;
    CHECK(stat("/dev/shm", &st) == 0 && S_ISDIR(st.st_mode), "/dev/shm exists");
    sem_unlink("/tosem");
    sem_t *s = sem_open("/tosem", O_CREAT | O_EXCL, 0644, 1);
    CHECK(s != SEM_FAILED, "sem_open creates a named semaphore");
    if (s == SEM_FAILED) return 1;
    CHECK(sem_wait(s) == 0, "sem_wait takes it");
    CHECK(sem_trywait(s) == -1 && errno == EAGAIN, "a second sem_trywait: EAGAIN");
    CHECK(sem_post(s) == 0, "sem_post gives it back");
    pid_t c = fork();
    if (c == 0) { sem_t *t = sem_open("/tosem", 0); if (t == SEM_FAILED) _exit(2); sem_wait(t); usleep(300000); sem_post(t); _exit(0); }
    usleep(100000);
    CHECK(sem_trywait(s) == -1 && errno == EAGAIN, "the child holds it (shared between processes)");
    int st2; waitpid(c, &st2, 0);
    CHECK(WIFEXITED(st2) && WEXITSTATUS(st2) == 0 && sem_trywait(s) == 0, "and it is free again afterwards");
    sem_close(s);
    CHECK(sem_unlink("/tosem") == 0, "sem_unlink");
    int fd = shm_open("/toshm", O_CREAT | O_RDWR, 0600);
    CHECK(fd >= 0 && ftruncate(fd, 4096) == 0, "shm_open and ftruncate");
    char *m = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(m != MAP_FAILED, "mmap MAP_SHARED of it");
    strcpy(m, "parent");
    c = fork();
    if (c == 0) { strcat(m, "+child"); _exit(0); }
    waitpid(c, &st2, 0);
    CHECK(!strcmp(m, "parent+child"), "a shared mapping is seen by both processes");
    shm_unlink("/toshm");
    printf(fails ? "semtest: %d FAILED\n" : "semtest: all passed\n", fails);
    return fails != 0;
}
