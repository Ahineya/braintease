/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>
static int output(int fd, const char *p, unsigned n) {
    ssize_t done;
    while (n) {
        done = write(fd, p, n);
        if (done < 0 && errno == EINTR) continue;
        if (done <= 0) return -1;
        n -= done; p += done;
    }
    return 0;
}
int main(int argc, char **argv) {
    struct timeval delay;
    char block[512];
    unsigned i, blocks;
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "wait")) {
        output(1, "BEFORE\n", 7);
        delay.tv_sec = 0; delay.tv_usec = 150000;
        select(0, 0, 0, 0, &delay);
        output(1, "AFTER\n", 6);
        return 7;
    }
    if (!strcmp(argv[1], "hang")) {
        if (argc > 2) {
            int fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0600);
            char number[16]; unsigned n = sizeof(number);
            unsigned long pid = getpid();
            do { number[--n] = '0' + pid % 10; pid /= 10; } while (pid);
            if (fd < 0 || output(fd, number + n, sizeof(number) - n) || close(fd)) return 3;
        }
        delay.tv_sec = 10; delay.tv_usec = 0;
        select(0, 0, 0, 0, &delay);
        return 0;
    }
    if (!strcmp(argv[1], "gone") && argc > 2) {
        int fd = open(argv[2], O_RDONLY);
        ssize_t n;
        unsigned long pid = 0;
        if (fd < 0) return 3;
        n = read(fd, block, sizeof(block)); close(fd);
        if (n <= 0) return 3;
        for (i = 0; i < (unsigned)n; ++i) {
            if (block[i] < '0' || block[i] > '9') return 3;
            pid = pid * 10 + block[i] - '0';
        }
        return kill(pid, 0) < 0 && errno == ESRCH ? 0 : 4;
    }
    if (!strcmp(argv[1], "signal")) { kill(getpid(), SIGTERM); return 99; }
    if (!strcmp(argv[1], "duplex") || !strcmp(argv[1], "overflow")) {
        memset(block, 'X', sizeof(block));
        blocks = !strcmp(argv[1], "overflow") ? 140 : 64;
        for (i = 0; i < blocks; ++i) {
            if (output(1, block, sizeof(block)) || output(2, block, sizeof(block))) return 3;
        }
        return 0;
    }
    for (i = 1; i < (unsigned)argc; ++i) {
        output(1, argv[i], strlen(argv[i])); output(1, "\n", 1);
    }
    return 0;
}
