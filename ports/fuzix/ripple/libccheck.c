/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
static int failed;
static jmp_buf jump;
static volatile int caught;
static void jump_back(void) { longjmp(jump, 23); }
static void catch_signal(int sig) { caught = sig; }
static void jump_signal(int sig) { longjmp(jump, sig); }
static int stack_jump(void) {
    jmp_buf local;
    int value = setjmp(local);
    if (!value)
        longjmp(local, 0);
    return value;
}
static void check(int ok, const char *s) {
    if (!ok) {
        printf("FAILED %s errno=%d\n", s, errno);
        failed = 1;
    }
}
static void inode_check(void) {
    char directory[40], path[64];
    unsigned i, count = 0, highest = 0;
    struct stat st;
    sprintf(directory, "/tmp/inode-check-%u", (unsigned)getpid());
    if (mkdir(directory, 0700) < 0) {
        check(0, "inode test directory");
        return;
    }
    for (i = 0; i < 320; i++) {
        int fd;
        sprintf(path, "%s/f%u", directory, i);
        fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0) break;
        count++;
        if (!fstat(fd, &st) && st.st_ino > highest) highest = st.st_ino;
        close(fd);
    }
    check(count == 320 && highest > 255, "allocate beyond 255 inodes");
    for (i = 0; i < count; i++) {
        sprintf(path, "%s/f%u", directory, i);
        check(unlink(path) == 0, "free high inode");
    }
    check(rmdir(directory) == 0, "inode directory cleanup");
}
static void buffer_check(void) {
    unsigned stack[2049], *heap = malloc(sizeof(stack));
    unsigned i;
    int fd;
    check(heap != NULL, "bulk buffer allocation");
    if (!heap) return;
    fd = open("/libc-bulk-test", O_RDWR | O_CREAT | O_TRUNC, 0600);
    check(fd >= 0, "bulk buffer file");
    if (fd >= 0) {
        for (i = 0; i < 2049; i++) heap[i] = i ^ 0xa55aU;
        check(write(fd, heap, sizeof(stack)) == sizeof(stack), "GP buffer write");
        check(lseek(fd, 0, SEEK_SET) == 0, "bulk buffer rewind");
        check(read(fd, stack, sizeof(stack)) == sizeof(stack), "SB buffer read");
        check(!memcmp(heap, stack, sizeof(stack)), "native words across filesystem blocks");
        for (i = 0; i < 2049; i++) stack[i] = i ^ 0x5aa5U;
        check(lseek(fd, 0, SEEK_SET) == 0, "bulk buffer rewrite seek");
        check(write(fd, stack, sizeof(stack)) == sizeof(stack), "SB buffer write");
        check(lseek(fd, 0, SEEK_SET) == 0, "bulk buffer reread seek");
        check(read(fd, heap, sizeof(stack)) == sizeof(stack), "GP buffer read");
        check(!memcmp(heap, stack, sizeof(stack)), "reverse native word transfer");
        check(close(fd) == 0, "bulk buffer close");
        check(unlink("/libc-bulk-test") == 0, "bulk buffer cleanup");
    }
    free(heap);
}
int main(void) {
    char b[80], small[4], *p, *q;
    FILE *f;
    struct stat st;
    DIR *d;
    struct dirent *e;
    int found = 0;
    long before;
    int jumped = setjmp(jump);
    if (!jumped)
        jump_back();
    check(jumped == 23, "setjmp/longjmp");
    check(stack_jump() == 1, "stack jmp_buf and longjmp zero");
    jumped = setjmp(jump);
    if (!jumped) {
        signal(SIGUSR2, jump_signal);
        kill(getpid(), SIGUSR2);
        check(0, "signal longjmp did not jump");
    }
    check(jumped == SIGUSR2, "longjmp out of signal handler");
    check(signal(SIGUSR1, catch_signal) != SIG_ERR, "install signal handler");
    check(kill(getpid(), SIGUSR1) == 0 && caught == SIGUSR1,
          "caught signal and return");
    caught = 0;
    check(signal(SIGALRM, catch_signal) != SIG_ERR, "install timer handler");
    alarm(1);
    while (!caught)
        ;
    check(caught == SIGALRM, "asynchronous user signal");

    before = (long)sbrk(0);
    p = malloc(7000);
    check(p != NULL, "malloc");
    if (p) {
        memset(p, 'x', 7000);
        q = realloc(p, 9000);
        check(q != NULL, "realloc");
        if (q) {
            check(q[0] == 'x' && q[6999] == 'x', "realloc data");
            free(q);
        } else
            free(p);
    }
    check((long)sbrk(0) >= before, "banked brk");
    check(snprintf(small, sizeof(small), "hello") == 5 && !strcmp(small, "hel"),
          "bounded printf");
    snprintf(b, sizeof(b), "%ld %lu %04x %.3s", -2147483647L, 4294967295UL, 42,
             "abcdef");
    check(!strcmp(b, "-2147483647 4294967295 002a abc"),
          "long variadic formatting");
    check(fprintf(stdout, "%s%s%s%s%s%s", "v", "a", "r", "a", "r", "gs\n") == 8,
          "many pointer arguments");
    f = fopen("/libc-test", "w+");
    check(f != NULL, "fopen");
    if (f) {
        check(fputs("alpha\nbeta\n", f) >= 0, "fputs");
        check(ftell(f) == 11, "buffered ftell");
        check(fseek(f, 0, SEEK_SET) == 0, "fseek");
        check(fgets(b, sizeof(b), f) != NULL && !strcmp(b, "alpha\n"), "fgets");
        check(ftell(f) == 6, "read ftell");
        check(ungetc('Z', f) == 'Z' && fgetc(f) == 'Z', "ungetc");
        check(fstat(fileno(f), &st) == 0 && st.st_size == 11 &&
                  S_ISREG(st.st_mode),
              "stat wire layout");
        check(fclose(f) == 0, "fclose");
    }
    d = opendir("/");
    check(d != NULL, "opendir");
    if (d) {
        while ((e = readdir(d)) != NULL)
            if (!strcmp(e->d_name, "libc-test"))
                found = 1;
        closedir(d);
    }
    check(found, "readdir wire layout");
    check(unlink("/libc-test") == 0, "unlink");
    p = malloc(512);
    check(p != NULL, "small malloc");
    if (p) {
        int fd = open("/heap-test", O_RDWR | O_CREAT | O_TRUNC, 0600);
        check(fd >= 0, "heap file");
        if (fd >= 0) {
            memset(p, 'H', 512);
            check(write(fd, p, 512) == 512, "heap write");
            lseek(fd, 0, SEEK_SET);
            check(read(fd, p, 512) == 512, "heap read");
            close(fd);
            unlink("/heap-test");
        }
        {
            int status;
            pid_t child = fork();
            check(child >= 0, "heap fork");
            if (!child) {
                p[0] = 'C';
                free(p);
                _exit(7);
            }
            if (child > 0) {
                check(waitpid(child, &status, 0) == child &&
                          WIFEXITED(status) && WEXITSTATUS(status) == 7,
                      "heap child");
                check(p[0] == 'H', "heap fork isolation");
            }
        }
        free(p);
    }
    buffer_check();
    inode_check();
    if (!failed)
        puts("libccheck passed");
    return failed;
}
