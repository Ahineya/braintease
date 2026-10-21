/* Opt-in native read groups: wide values, page edges, heap, fork and exec. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

static int initial = 7;
static unsigned long sum4(unsigned long *p)
{
    return p[0] + p[1] + p[2] + p[3];
}
static unsigned long long wide2(unsigned long long *p)
{
    return p[0] ^ p[1];
}

int main(int argc, char **argv)
{
    unsigned long *p;
    unsigned long long wide[2];
    int child, status;
    (void)argv;
    if (argc > 1)
        return initial == 7 ? 23 : 90;
    p = malloc(2097152UL);
    if (!p) return 1;
    for (unsigned i = 0; i < 2048; i++) p[i] = i;
    wide[0] = 0x123456789abcdef0ULL;
    wide[1] = 0xfedcba9876543210ULL;
    if (wide2(wide) != 0xece8ece0ece8ece0ULL) return 2;
    if (sum4(p) != 6 || sum4(p + 254) != 1022) return 3;
    child = fork();
    if (child < 0) return 4;
    if (!child) {
        p[0] = 100;
        _exit(sum4(p) == 106 ? 17 : 91);
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)
        || WEXITSTATUS(status) != 17 || sum4(p) != 6) return 5;
    initial = 99;
    child = fork();
    if (child < 0) return 6;
    if (!child) {
        char *args[] = { "memviewcheck", "exec", NULL };
        execv("/bin/memviewcheck", args);
        _exit(92);
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)
        || WEXITSTATUS(status) != 23 || initial != 99) return 7;
    for (unsigned long i = 0; i < 100000UL; i++)
        if (sum4(p + (i & 1023)) != 4 * (i & 1023) + 6) return 8;
    free(p);
    puts("MEMVIEW_GUEST_OK");
    return 0;
}
