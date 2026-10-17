/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A minimal PTY host: the child owns a separate session and controlling tty. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <signal.h>
#include <sys/wait.h>
#include "pty-limits.h"
static struct termios saved;
static int changed, master = -1, child;
static volatile int interrupted;
static void caught(int sig) { interrupted = sig; signal(sig, caught); }
static void cleanup(void) {
    if (changed) tcsetattr(0, TCSANOW, &saved);
    if (master >= 0) close(master);
}
int main(int argc, char **argv) {
    char path[] = "/dev/ptyp0", slave[] = "/dev/ttyp0";
    char output[256], input[256];
    char *sh[] = {"sh", NULL};
    unsigned i, pending = 0, sent = 0;
    int n, status = 0, result = 0;
    struct termios raw;
    struct winsize size;
    if (argc > 1 && !strcmp(argv[1], "--help")) {
        puts("usage: ptysh [COMMAND [ARG...]]\nRun a command (default sh) in a separate terminal session."); return 0;
    }
    if (!isatty(0) || tcgetattr(0, &saved) < 0) { fputs("ptysh: interactive terminal required\n", stderr); return 1; }
    for (i = 0; i < RIPPLE_PTY_COUNT; i++) {
        path[9] = slave[9] = '0' + i;
        master = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (master >= 0) break;
        if (errno != EBUSY) { perror("ptysh"); return 1; }
    }
    if (master < 0) { fputs("ptysh: no free terminals\n", stderr); return 1; }
    atexit(cleanup);
    if (ioctl(0, TIOCGWINSZ, &size) == 0 && size.ws_row && size.ws_col)
        ioctl(master, TIOCSWINSZ, &size);
    fflush(NULL);
    child = fork();
    if (child < 0) { perror("ptysh: fork"); return 1; }
    if (!child) {
        int fd;
        close(master);
        if (setsid() < 0) _exit(125);
        fd = open(slave, O_RDWR);
        if (fd < 0) _exit(125);
        dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
        if (fd > 2) close(fd);
        /* Do not retain the outer shell's safe tty descriptor. */
        for (i = 3; i < 16; i++) close(i);
        execvp(argc > 1 ? argv[1] : "/bin/sh", argc > 1 ? argv + 1 : sh);
        _exit(127);
    }
    raw = saved; raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(ICRNL | INLCR | IXON);
    raw.c_oflag &= ~OPOST; raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 1;
    if (tcsetattr(0, TCSANOW, &raw) < 0) { kill(child, SIGKILL); waitpid(child, &status, 0); return 1; }
    changed = 1;
    signal(SIGINT, caught); signal(SIGTERM, caught); signal(SIGHUP, caught);
    for (;;) {
        n = read(master, output, sizeof(output));
        if (n > 0) {
            int off = 0, written;
            while (off < n) {
                written = write(1, output + off, n - off);
                if (written <= 0) { result = 1; interrupted = SIGHUP; break; }
                off += written;
            }
        } else if (!n) break;
        else if (errno != EAGAIN && errno != EINTR) { result = 1; break; }
        if (interrupted) break;
        if (!pending) {
            n = read(0, input, sizeof(input));
            if (n > 0) { pending = n; sent = 0; }
            else if (n < 0 && errno != EINTR && errno != EAGAIN) { result = 1; break; }
        }
        if (pending) {
            n = write(master, input + sent, pending);
            if (n > 0) { sent += n; pending -= n; }
            else if (n < 0 && errno != EINTR && errno != EAGAIN) { result = 1; break; }
        }
        /* Reap an early failure (before the slave was opened) as well. */
        if (waitpid(child, &status, WNOHANG) == child) {
            while ((n = read(master, output, sizeof(output))) > 0) write(1, output, n);
            child = 0; break;
        }
    }
    if (child) {
        kill(-child, SIGHUP); kill(-child, SIGCONT);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }
    return result ? result : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
