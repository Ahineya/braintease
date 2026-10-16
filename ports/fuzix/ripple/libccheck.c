/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static int failed;
static void check(int ok, const char *s) {
    if (!ok) {
        printf("FAILED %s errno=%d\n", s, errno);
        failed = 1;
    }
}
int main(void) {
    char b[80], small[4], *p, *q;
    FILE *f;
    struct stat st;
    DIR *d;
    struct dirent *e;
    int found = 0;
    long before;

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
    if (!failed)
        puts("libccheck passed");
    return failed;
}
