/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real kernel/libc acceptance: no host descriptors or networking shortcuts. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern long call(unsigned, long, long, long, long);
static const char *program;
#undef select
extern int select(int, struct ripple_fd_set *, struct ripple_fd_set *,
                  struct ripple_fd_set *, struct timeval *);
static int legacy_select(int n, struct ripple_fd_set *r, struct timeval *t) {
    return select(n, r, NULL, NULL, t);
}
#define select ripple_select_wide
static void check(int ok, const char *what) {
    if (!ok) { fprintf(stderr, "FD_FAILED %s errno=%d\n", what, errno); exit(1); }
}
static void child_ok(pid_t p) {
    int status;
    check(p > 0 && waitpid(p, &status, 0) == p && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0, "child status");
}
static void exact(int fd, void *p, unsigned n) {
    char *out = p;
    while (n) { int got = read(fd, out, n); check(got > 0, "report read"); out += got; n -= got; }
}
static void stream_checks(void) {
    FILE *files[128], *f, *saved;
    int i, pass, n, error;
    check(sysconf(_SC_OPEN_MAX) == 128 && FD_SETSIZE == 128, "descriptor capacity");
    check(sizeof(FILE) == 267, "existing C16 FILE ABI");
    check(sysconf(_SC_STREAM_MAX) >= 128, "stream capacity report");
    for (pass = 0; pass < 2; pass++) {
        for (n = 0; n < 128; n++) {
            files[n] = fopen("/dev/null", "w");
            if (!files[n]) break;
            check(fputc('a', files[n]) == 'a', "buffer stream");
        }
        error = errno;
        check(n == 125 && error == EMFILE, "all 128 handles usable");
        check(fflush(NULL) == 0, "flush all dynamic streams");
        check(fclose(files[n-1]) == 0, "release highest stream");
        i = open("/dev/null", O_WRONLY | O_CLOEXEC);
        check(i == 127 && fcntl(i, F_GETFD) == FD_CLOEXEC, "open sets highest close-on-exec bit");
        files[n-1] = fdopen(i, "w"); check(files[n-1] != NULL, "wrap highest descriptor");
        saved = files[40];
        check(freopen("/etc/resolv.conf", "r", saved) == saved, "freopen preserves FILE address");
        check(fgetc(saved) == 'n', "freopened stream input");
        for (i = n - 1; i >= 0; i--) check(fclose(files[i]) == 0, "release streams");
    }
    f = fopen("/dev/null", "r"); check(f != NULL, "open for failed reopen");
    check(freopen("/missing-fd-test", "r", f) == NULL, "failed reopen");
    check(fdopen(-1, "r") == NULL && errno == EBADF, "negative fdopen");
    puts("FD_STREAMS_OK");
}
static void select_checks(void) {
    int p[2], s[2], high = 127, fd, result;
    char c;
    fd_set r, w;
    struct timeval zero = {0, 0};
    struct timeval wait = {2, 0};
    pid_t child;
    struct { struct ripple_fd_set set; unsigned guard[8]; } legacy;
    struct ripple_select_request request;
    unsigned i;
    check(pipe(p) == 0 && dup2(p[0], high) == high, "high pipe"); close(p[0]);
    FD_ZERO(&r); FD_SET(high, &r);
    check(select(128, &r, NULL, NULL, &zero) == 0 && !FD_ISSET(high, &r), "high empty pipe");
    child = fork(); check(child >= 0, "high select writer fork");
    if (!child) {
        struct timeval delay = {0, 50000};
        if (select(0, NULL, NULL, NULL, &delay) || write(p[1], "w", 1) != 1) _exit(93);
        _exit(0);
    }
    FD_ZERO(&r); FD_SET(high, &r);
    check(select(128, &r, NULL, NULL, &wait) == 1 && FD_ISSET(high, &r) &&
          read(high, &c, 1) == 1 && c == 'w', "blocked high select wakeup");
    child_ok(child);
    check(write(p[1], "z", 1) == 1, "pipe data");
    FD_ZERO(&r); FD_SET(high, &r);
    check(select(128, &r, NULL, NULL, &zero) == 1 && FD_ISSET(high, &r), "high readable pipe");
    check(read(high, &c, 1) == 1 && c == 'z', "high pipe read"); close(p[1]);
    FD_ZERO(&r); FD_SET(high, &r);
    check(select(128, &r, NULL, NULL, &zero) == 1 && read(high, &c, 1) == 0, "high pipe EOF");
    close(high);
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0, "socketpair");
    check(dup2(s[0], 96) == 96, "high socket"); close(s[0]);
    FD_ZERO(&r); FD_SET(96, &r);
    check(select(97, &r, NULL, NULL, &zero) == 0, "high empty socket");
    check(write(s[1], "s", 1) == 1, "socket data");
    FD_ZERO(&r); FD_SET(96, &r);
    check(select(97, &r, NULL, NULL, &zero) == 1 && FD_ISSET(96, &r) &&
          read(96, &c, 1) == 1 && c == 's', "high readable socket");
    close(s[1]); close(96);
    fd = open("/dev/null", O_RDWR); check(fd >= 0, "legacy descriptor");
    memset(&legacy, 0, sizeof(legacy));
    for (i = 0; i < 8; i++) legacy.guard[i] = 0xa500U + i;
    legacy.set.bits[fd >> 4] = 1U << (fd & 15);
    request.version = RIPPLE_SELECT_VERSION; request.sets[0] = &legacy.set;
    request.sets[1] = request.sets[2] = NULL; request.timeout = &zero;
    check(call(72, fd + 1, (long)&request, 0, 0) == 1, "old two-word select");
    check(legacy_select(fd + 1, &legacy.set, &zero) == 1, "legacy libc select symbol");
    for (i = 0; i < 8; i++) check(legacy.guard[i] == 0xa500U + i, "legacy copy boundary");
    check(call(72, 33, (long)&request, 0, 0) == -1 && errno == EINVAL, "legacy nfds bound");
    check(legacy_select(33, &legacy.set, &zero) == -1 && errno == EINVAL, "legacy libc nfds bound");
    request.version = 99;
    check(call(72, 0, (long)&request, 0, 0) == -1 && errno == EINVAL, "unknown select version");
    check(dup2(fd, high) == high, "highest regular fd");
    FD_ZERO(&r); FD_ZERO(&w); FD_SET(high, &r); FD_SET(high, &w);
    result = select(128, &r, &w, NULL, &zero);
    check(result == 2 && FD_ISSET(high, &r) && FD_ISSET(high, &w), "high read and write sets");
    close(high); close(fd);
    FD_ZERO(&r); FD_SET(high, &r);
    check(select(128, &r, NULL, NULL, &zero) == -1 && errno == EBADF, "closed high select");
    check(select(129, NULL, NULL, NULL, &zero) == -1 && errno == EINVAL, "wide nfds bound");
    puts("FD_SELECT_OK");
}
static void flush_child(void) {
    int i; char path[40]; FILE *f;
    for (i = 0; i < 24; i++) {
        sprintf(path, "/tmp/fd-exit-%d", i); f = fopen(path, "w");
        check(f != NULL && fputc('A' + i, f) == 'A' + i, "child buffers");
    }
    /* main returns through libc exit; no stream is closed explicitly. */
}
static void flush_checks(void) {
    FILE *files[3], *f;
    char path[40]; int i; pid_t child;
    char *args[] = {(char *)program, "flush-child", NULL};
    for (i = 0; i < 3; i++) {
        sprintf(path, "/tmp/fd-flush-%d", i); files[i] = fopen(path, "w+");
        check(files[i] != NULL && fputc('a' + i, files[i]) == 'a' + i, "flush buffers");
    }
    check(close(fileno(files[1])) == 0, "inject stream write error");
    check(fflush(NULL) == EOF && ferror(files[1]), "flush all reports error");
    check(fclose(files[1]) == EOF, "close failed stream");
    for (i = 0; i < 3; i++) {
        if (i != 1) {
            check(fseek(files[i], 0, SEEK_SET) == 0 && fgetc(files[i]) == 'a' + i,
                  "flush continues past failed stream");
            check(fclose(files[i]) == 0, "close good stream");
        }
        sprintf(path, "/tmp/fd-flush-%d", i); check(unlink(path) == 0, "remove flush fixture");
    }
    child = fork(); check(child >= 0, "flush child fork");
    if (!child) { execvp(program, args); _exit(94); }
    child_ok(child);
    for (i = 0; i < 24; i++) {
        sprintf(path, "/tmp/fd-exit-%d", i); f = fopen(path, "r");
        check(f != NULL && fgetc(f) == 'A' + i && fgetc(f) == EOF, "exit flushes dynamic stream");
        fclose(f); check(unlink(path) == 0, "remove exit fixture");
    }
    puts("FD_FLUSH_OK");
}
static int boundaries[] = {31, 32, 63, 64, 95, 96, 126, 127};
static void exec_child(void) {
    unsigned i;
    for (i = 0; i < sizeof(boundaries)/sizeof(boundaries[0]); i++) {
        int flags = fcntl(boundaries[i], F_GETFD);
        check((i & 1) ? flags == 0 : flags == -1 && errno == EBADF, "exec high descriptor flags");
    }
}
static void lifecycle_checks(void) {
    int fd = open("/dev/null", O_RDONLY), status;
    unsigned i;
    pid_t child;
    char *args[] = {(char *)program, "exec-child", NULL};
    check(fd >= 0, "lifecycle source");
    for (i = 0; i < sizeof(boundaries)/sizeof(boundaries[0]); i++) {
        int target = boundaries[i];
        check(dup2(fd, target) == target && fcntl(target, F_GETFD) == 0, "dup high descriptor");
        check(fcntl(target, F_SETFD, (i & 1) ? 0 : FD_CLOEXEC) == 0, "set high close-on-exec");
    }
    check(fcntl(31, F_DUPFD, 100) == 100 && fcntl(100, F_GETFD) == 0, "F_DUPFD clears flags"); close(100);
    status = dup(31); check(status >= 0 && fcntl(status, F_GETFD) == 0, "dup flagged fd"); close(status);
    child = fork(); check(child >= 0, "fork high descriptors");
    if (!child) {
        for (i = 0; i < sizeof(boundaries)/sizeof(boundaries[0]); i++)
            check(fcntl(boundaries[i], F_GETFD) == ((i & 1) ? 0 : FD_CLOEXEC), "fork flags preserved");
        execvp(program, args); perror("fdcheck exec"); _exit(90);
    }
    child_ok(child);
    for (i = 0; i < sizeof(boundaries)/sizeof(boundaries[0]); i++) {
        check(fcntl(boundaries[i], F_GETFD) == ((i & 1) ? 0 : FD_CLOEXEC), "parent flags preserved");
        check(close(boundaries[i]) == 0, "close boundary fd");
    }
    close(fd);
    /* No wait-all shortcut: popen must retain each particular child's PID. */
    {
        FILE *pipes[20];
        for (i = 0; i < 20; i++) { pipes[i] = popen("echo p", "r"); check(pipes[i] != NULL, "20 concurrent popen streams"); }
        for (i = 20; i; ) {
            --i; check(fgetc(pipes[i]) == 'p', "popen output");
            status = pclose(pipes[i]); check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "pclose particular child");
        }
    }
    puts("FD_LIFECYCLE_OK");
}
static void global_checks(void) {
    int reports[2], release[2], i, fd, total = 0, saw_full = 0;
    pid_t children[10];
    unsigned result[2];
    char token;
    check(pipe(reports) == 0 && pipe(release) == 0, "global controls");
    for (i = 0; i < 10; i++) {
        children[i] = fork(); check(children[i] >= 0, "global worker fork");
        if (!children[i]) {
            unsigned count = 0;
            close(reports[0]); close(release[1]);
            while (count < 110 && open("/dev/null", O_RDONLY) >= 0) count++;
            result[0] = count; result[1] = count == 110 ? 0 : errno;
            if (write(reports[1], result, sizeof(result)) != sizeof(result)) _exit(91);
            if (read(release[0], &token, 1) != 1) _exit(92);
            _exit(0); /* kernel must reclaim all handles, including high OFT indices */
        }
        exact(reports[0], result, sizeof(result));
        total += result[0];
        check(result[1] == 0 || result[1] == ENFILE, "global exhaustion error");
        if (result[1] == ENFILE) saw_full = 1;
    }
    check(saw_full && total >= 1000, "1024 global entries usable");
    check(open("/dev/null", O_RDONLY) == -1 && errno == ENFILE, "global ENFILE distinct from EMFILE");
    for (i = 0; i < 10; i++) check(write(release[1], "x", 1) == 1, "release global workers");
    for (i = 0; i < 10; i++) child_ok(children[i]);
    close(reports[0]); close(reports[1]); close(release[0]); close(release[1]);
    fd = open("/dev/null", O_RDONLY); check(fd >= 0 && close(fd) == 0, "global exhaustion recovery");
    puts("FD_GLOBAL_OK");
}
int main(int argc, char **argv) {
    program = argv[0];
    if (argc > 1 && !strcmp(argv[1], "exec-child")) { exec_child(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "flush-child")) { flush_child(); return 0; }
    stream_checks(); select_checks(); flush_checks(); lifecycle_checks(); global_checks(); global_checks();
    puts("FD_LIMITS_OK"); return 0;
}
