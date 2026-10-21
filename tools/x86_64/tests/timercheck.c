/* A syscall-free busy parent must not starve a timed child or signal delivery.
 * This uses the normal independent application and kernel syscall paths. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

static volatile sig_atomic_t notified;
static void notify(int sig) { notified = sig == SIGUSR1 ? 1 : 2; }
static void check(int condition) {
    if (!condition) { puts("TIMER CHECK FAILED"); exit(1); }
}
int main(void) {
    int fd[2], status, owner = getpid();
    check(pipe(fd) == 0);
    check(signal(SIGUSR1, notify) != SIG_ERR && signal(SIGALRM, notify) != SIG_ERR);
    int child = fork();
    check(child >= 0);
    if (!child) {
        close(fd[0]);
        sleep(1);
        if (write(fd[1], "T", 1) != 1 || kill(owner, SIGUSR1)) _exit(7);
        close(fd[1]);
        _exit(23);
    }
    close(fd[1]);
    alarm(8);
    volatile unsigned long iterations = 0;
    /* No sleep, read, yield, time sample or other syscall inside this loop. */
    while (!notified) ++iterations;
    alarm(0);
    check(notified == 1 && iterations > 0);
    char result = 0;
    check(read(fd[0], &result, 1) == 1 && result == 'T');
    close(fd[0]);
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 23);
    puts("timer checks passed (busy loop, sleeping child, signal and wait)");
    return 0;
}
