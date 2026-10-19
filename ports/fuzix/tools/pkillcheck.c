/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real exec/signal coverage; child readiness uses a pipe, never a timing guess. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

static unsigned failures;
static void check(int good, const char *what) {
    if (!good) { printf("PKILL_FAILED %s\n", what); failures++; }
}
static int sleeper(const char *name) {
    int fds[2], pid;
    char number[12], ready;
    char *args[4];
    if (pipe(fds)) return -1;
    pid = fork();
    if (!pid) {
        close(fds[0]);
        sprintf(number, "%d", fds[1]);
        args[0] = (char *)name; args[1] = "--wait"; args[2] = number; args[3] = NULL;
        execv("/bin/pkillcheck", args);
        _exit(125);
    }
    close(fds[1]);
    if (pid < 0 || read(fds[0], &ready, 1) != 1 || ready != 'R') {
        close(fds[0]); return -1;
    }
    close(fds[0]); return pid;
}
static int invoke(const char *a, const char *b, const char *c, const char *d) {
    int pid, status;
    char *args[6];
    args[0] = "pkill"; args[1] = (char *)a; args[2] = (char *)b;
    args[3] = (char *)c; args[4] = (char *)d; args[5] = NULL;
    pid = fork();
    if (!pid) { execv("/bin/pkill", args); _exit(125); }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}
static int ended(int pid, int signal) {
    int status;
    if (pid < 0 || waitpid(pid, &status, 0) != pid) return 0;
    return WIFSIGNALED(status) && WTERMSIG(status) == signal;
}
static int alive(int pid) {
    int status;
    return pid > 1 && waitpid(pid, &status, WNOHANG) == 0;
}
int main(int argc, char **argv) {
    int a, b, other, status;
    if (argc == 3 && !strcmp(argv[1], "--wait")) {
        int fd = atoi(argv[2]);
        if (write(fd, "R", 1) != 1) return 125;
        close(fd);
        for (;;) pause();
    }
    a = sleeper("napA"); b = sleeper("napAB"); other = sleeper("stay");
    if (a < 0 || b < 0 || other < 0) { puts("PKILL_FAILED child startup"); return 1; }
    check(invoke("-0", "-x", "napA", NULL) == 0, "signal zero");
    check(invoke("-0", "-x", "nap", NULL) == 1, "exact excludes prefix");
    check(invoke("-0", "nap", NULL, NULL) == 0, "substring match");
    check(alive(a) && alive(b) && alive(other), "probe preserves processes");
    check(invoke("-TERM", "-x", "napA", NULL) == 0, "symbolic TERM");
    check(ended(a, SIGTERM) && alive(b) && alive(other), "exact only kills selected");
    check(invoke("--signal=STOP", "-x", "napAB", NULL) == 0, "STOP");
    check(waitpid(b, &status, WUNTRACED) == b && WIFSTOPPED(status), "child stopped");
    check(invoke("-SIGCONT", "-x", "napAB", NULL) == 0, "CONT");
    check(invoke("-9", "-e", "^nap(AB|Z)$", NULL) == 0, "numeric and regex");
    check(ended(b, SIGKILL) && alive(other), "regex leaves unrelated process");
    check(invoke("-x", "stay", NULL, NULL) == 0 && ended(other, SIGTERM), "default TERM");
    check(invoke("-0", "^pkill$", NULL, NULL) == 1, "never matches itself");
    check(invoke("no-such-process", NULL, NULL, NULL) == 1, "no match status");
    a = sleeper("bothA"); b = sleeper("bothB");
    check(invoke("^both", NULL, NULL, NULL) == 0, "multiple matches");
    check(ended(a, SIGTERM) && ended(b, SIGTERM), "all matching processes signalled");
    a = sleeper("abcdefghijk");
    check(invoke("-0", "-x", "abcdefghijk", NULL) == 1, "documented truncation");
    check(invoke("-x", "abcdefgh", NULL, NULL) == 0 && ended(a, SIGTERM), "eight character name");
    a = sleeper("-dash");
    check(invoke("--signal", "term", "--", "-dash") == 0 && ended(a, SIGTERM), "dash pattern and signal option");
    check(invoke("[", NULL, NULL, NULL) == 2, "invalid regex");
    check(invoke("-32", "nap", NULL, NULL) == 2, "invalid signal");
    check(invoke("--signal", "9999999999999999", "nap", NULL) == 2, "signal overflow");
    check(invoke("--signal", "TERM", NULL, NULL) == 2, "missing pattern");
    check(invoke("", NULL, NULL, NULL) == 2, "empty pattern");
    check(invoke("nap", "extra", NULL, NULL) == 2, "extra argument");
    check(invoke("--help", NULL, NULL, NULL) == 0, "help");
    check(invoke("-l", NULL, NULL, NULL) == 0, "signal list");
    printf("PKILL_%s %u failures\n", failures ? "FAILED" : "OK", failures);
    return failures != 0;
}
