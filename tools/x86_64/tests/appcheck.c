#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <setjmp.h>
#include <errno.h>
#include <fcntl.h>

static int global = 7;
static unsigned char large[70000];
static unsigned char *far = large + 64001;
static jmp_buf target;
static int twice(int value) { return value * 2; }
static int (*callback)(int) = twice;
static void jump(int value) { longjmp(target, value); }
static void check(int condition) {
    if (!condition) { puts("APP CHECK FAILED"); exit(1); }
}
static void inherited_jump(void) {
    jmp_buf saved;
    volatile int owner = getpid(), memory = 17;
    int value = setjmp(saved);
    if (value) {
        check(value == 9 && getpid() != owner && memory == 17);
        _exit(29);
    }
    int pid = fork(), status;
    check(pid >= 0);
    if (!pid) longjmp(saved, 9);
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 29);
}
int main(int argc, char **argv) {
    check(global == 7 && callback(21) == 42 && getenv("PATH") != NULL && *far == 0 && large[69999] == 0);
    if (argc == 2 && !strcmp(argv[1], "--exec")) {
        puts("independent exec reset passed"); return 37;
    }
    inherited_jump();
    *far = 73;
    unsigned char *heap = malloc(80000UL);
    check(heap != NULL);
    check(((unsigned long)heap >> 16) >= 48 && ((unsigned long)heap >> 16) < 240);
    check(((unsigned long)(heap + 79999UL) >> 16) < 240);
    /* The kernel must use the same profile as libc: unsupported high-bank
     * pointers are rejected before a user-copy reaches the native runtime. */
    check(write(1, (const void *)0x01000000UL, 1) == -1 && errno == EFAULT);
    check(brk((void *)0x01000000UL) == -1 && errno == ENOMEM);
    heap[0] = 31; heap[79999UL] = 47;
    volatile int memory = 11;
    int result = setjmp(target);
    if (!result) { memory = 23; global = 19; jump(0); }
    check(result == 1 && memory == 23 && global == 19);
    for (int round = 0; round < 4; ++round) {
        int pid = fork(), status;
        check(pid >= 0);
        if (!pid) {
            check(global == 19 && memory == 23 && callback(3) == 6 && *far == 73 &&
                  heap[0] == 31 && heap[79999UL] == 47);
            *far = 99; heap[79999UL] = 99;
            /* A copied jump target still names this child's continuation. */
            if (round == 0) {
                int value = setjmp(target);
                if (!value) jump(9);
                check(value == 9);
            }
            global = 99;
            char *next[] = {"appcheck", "--exec", NULL};
            execv("/bin/appcheck", next);
            exit(90);
        }
        check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 37);
        check(global == 19 && memory == 23 && *far == 73 && heap[79999UL] == 47);
    }
    char *args[] = {"missing", NULL};
    check(execv("/missing", args) == -1 && errno == ENOENT && global == 19 && callback(4) == 8);
    unsigned char invalid[64];
    memset(invalid, 0, sizeof(invalid)); memcpy(invalid, "FUSEX001", 8);
    int fd = open("/tmp/bad-image", O_CREAT | O_TRUNC | O_WRONLY, 0755);
    check(fd >= 0 && write(fd, invalid, sizeof(invalid)) == sizeof(invalid) && close(fd) == 0);
    check(execv("/tmp/bad-image", args) == -1 && errno == ENOEXEC && global == 19 && *far == 73);
    /* Image preparation succeeds before the kernel validates argv. */
    check(execv("/bin/echo", (char **)0xffffffffUL) == -1 && errno == EFAULT &&
          global == 19 && *far == 73 && callback(4) == 8);
    free(heap);
    puts("independent app checks passed (callbacks, setjmp, fork, exec, wait and rollback)");
    return 0;
}
