/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
extern long call(unsigned, long, long, long, long);
int errno;
char **environ;
int _argc;
char **_argv;
extern void ripple_stdio_init(void);
void ripple_libc_init(int argc, char **argv, char **envp) {
    ripple_stdio_init();
    _argc = argc;
    _argv = argv;
    environ = envp;
}
void _exit(int status) {
    call(0, status, 0, 0, 0);
    for (;;)
        ;
}
static atexit_t handlers[16];
static unsigned nhandlers;
int atexit(atexit_t f) {
    if (nhandlers == 16)
        return -1;
    handlers[nhandlers++] = f;
    return 0;
}
void exit(int status) {
    while (nhandlers)
        handlers[--nhandlers]();
    if (fflush(NULL) < 0 && !status)
        status = 1;
    _exit(status);
}
void abort(void) { _exit(134); }
int creat(const char *p, mode_t m) {
    return open(p, O_WRONLY | O_CREAT | O_TRUNC, m);
}
int rename(const char *p, const char *q) {
    return call(3, (long)p, (long)q, 0, 0);
}
int mknod(const char *p, mode_t m, dev_t d) {
    return call(4, (long)p, m, d, 0);
}
int link(const char *p, const char *q) {
    return call(5, (long)p, (long)q, 0, 0);
}
int unlink(const char *p) { return call(6, (long)p, 0, 0, 0); }
off_t lseek(int fd, off_t off, int whence) {
    if (call(9, fd, (long)&off, whence, 0) < 0)
        return -1;
    return off;
}
int chdir(const char *p) { return call(10, (long)p, 0, 0, 0); }
void sync(void) { call(11, 0, 0, 0, 0); }
int access(const char *p, int m) { return call(12, (long)p, m, 0, 0); }
int chmod(const char *p, mode_t m) { return call(13, (long)p, m, 0, 0); }
int fchmod(int fd, mode_t m) { return call(49, fd, m, 0, 0); }
int chown(const char *p, uid_t u, gid_t g) {
    return call(14, (long)p, u, g, 0);
}
static void convert_stat(struct stat *s, struct _uzistat *k) {
    s->st_dev = k->st_dev;
    s->st_ino = k->st_ino;
    s->st_mode = k->st_mode;
    s->st_nlink = k->st_nlink;
    s->st_uid = k->st_uid;
    s->st_gid = k->st_gid;
    s->st_rdev = k->st_rdev;
    s->st_size = k->st_size;
    s->st_atime = k->st_atime;
    s->st_mtime = k->st_mtime;
    s->st_ctime = k->st_ctime;
}
int stat(const char *p, struct stat *s) {
    struct _uzistat k;
    if (call(15, (long)p, (long)&k, 0, 0) < 0)
        return -1;
    convert_stat(s, &k);
    return 0;
}
int lstat(const char *p, struct stat *s) { return stat(p, s); }
int fstat(int fd, struct stat *s) {
    struct _uzistat k;
    if (call(16, fd, (long)&k, 0, 0) < 0)
        return -1;
    convert_stat(s, &k);
    return 0;
}
int dup(int fd) { return call(17, fd, 0, 0, 0); }
pid_t getpid(void) { return call(18, 0, 0, 0, 0); }
pid_t getpgrp(void) { return call(61, 0, 0, 0, 0); }
int setpgid(pid_t p, pid_t g) { return call(77, p, g, 0, 0); }
pid_t setsid(void) { return call(78, 0, 0, 0, 0); }
pid_t getsid(pid_t p) { return call(79, p, 0, 0, 0); }
pid_t setpgrp(void) { return setsid(); }
pid_t getppid(void) { return call(19, 0, 0, 0, 0); }
uid_t getuid(void) { return call(20, 0, 0, 0, 0); }
mode_t umask(mode_t m) { return call(21, m, 0, 0, 0); }
int execve(const char *p, char *const a[], char *const e[]) {
    return call(23, (long)p, (long)a, (long)e, 0);
}
int execv(const char *p, char *const a[]) { return execve(p, a, environ); }
int _getdirent(int fd, void *buf, size_t len) {
    return call(24, fd, (long)buf, len, 0);
}
int execvp(const char *p, char *const a[]) {
    char name[512];
    const char *search = getenv("PATH"), *end;
    size_t n;
    if (strchr(p, '/'))
        return execv(p, a);
    if (!search)
        search = "/bin:/usr/bin";
    do {
        end = strchr(search, ':');
        n = end ? end - search : strlen(search);
        if (n + strlen(p) + 2 <= sizeof(name)) {
            memcpy(name, search, n);
            if (n)
                name[n++] = '/';
            strcpy(name + n, p);
            execv(name, a);
            if (errno != ENOENT && errno != ENOTDIR)
                return -1;
        }
        search = end ? end + 1 : NULL;
    } while (search);
    errno = ENOENT;
    return -1;
}
int setuid(uid_t u) { return call(25, u, 0, 0, 0); }
int setgid(gid_t g) { return call(26, g, 0, 0, 0); }
time_t time(time_t *p) {
    __ktime_t t;
    if (call(27, (long)&t, 0, 0, 0) < 0)
        return (time_t)-1;
    if (p)
        *p = t.low;
    return t.low;
}
int utime(const char *p, struct utimbuf *t) {
    __ktime_t k[2];
    if (!t)
        return call(43, (long)p, 0, 0, 0);
    k[0].low = t->actime;
    k[0].high = 0;
    k[1].low = t->modtime;
    k[1].high = 0;
    return call(43, (long)p, (long)k, 0, 0);
}
int brk(void *p) { return call(30, (long)p, 0, 0, 0); }
void *sbrk(intptr_t n) { return (void *)call(31, n, 0, 0, 0); }
pid_t fork(void) { return call(32, 0, 0, 0, 0); }
int dup2(int a, int b) { return call(36, a, b, 0, 0); }
int kill(pid_t p, int s) { return call(39, p, s, 0, 0); }
int pipe(int *p) { return call(40, (long)p, 0, 0, 0); }
gid_t getgid(void) { return call(41, 0, 0, 0, 0); }
uid_t geteuid(void) { return call(44, 0, 0, 0, 0); }
gid_t getegid(void) { return call(45, 0, 0, 0, 0); }
int fchdir(int fd) { return call(48, fd, 0, 0, 0); }
int mkdir(const char *p, mode_t m) { return call(51, (long)p, m, 0, 0); }
int rmdir(const char *p) { return call(52, (long)p, 0, 0, 0); }
pid_t waitpid(pid_t p, int *s, int o) { return call(55, p, (long)s, o, 0); }
pid_t wait(int *s) { return waitpid(-1, s, 0); }
int uadmin(int c, int f, void *p) { return call(57, c, f, (long)p, 0); }
int nice(int p) { return call(58, p, 0, 0, 0); }
int ftruncate(int fd, off_t n) { return call(67, fd, (long)&n, 0, 0); }
unsigned sleep(unsigned s) {
    /* A zero kernel timeout means an indefinite pause. Keep positive
     * decisecond timeouts within the kernel's signed 16-bit argument. */
    while (s) {
        unsigned chunk = s > 3276 ? 3276 : s;
        if (call(37, (long)chunk * 10, 0, 0, 0) < 0)
            return s;
        s -= chunk;
    }
    return 0;
}
int isatty(int fd) {
    struct stat s;
    int saved = errno,
        r = fstat(fd, &s) == 0 && S_ISCHR(s.st_mode) && major(s.st_rdev) == 2;
    errno = saved;
    return r;
}
int remove(const char *p) {
    struct stat s;
    if (stat(p, &s) < 0)
        return -1;
    return S_ISDIR(s.st_mode) ? rmdir(p) : unlink(p);
}
sighandler_t signal(int n, sighandler_t f) {
    extern void ripple_signal_entry(void);
    return (sighandler_t)call(35, n, (long)f, (long)ripple_signal_entry, 0);
}
void ripple_sigreturn(void *frame) { call(100, (long)frame, 0, 0, 0); }
unsigned alarm(unsigned seconds) {
    unsigned ticks = seconds > 6553 ? 65535 : seconds * 10;
    long previous = call(38, ticks, 0, 0, 0);
    return previous < 0 ? 0 : (previous + 9) / 10;
}
int pause(void) { return call(37, 0, 0, 0, 0); }
int times(struct tms *t) { return call(42, (long)t, 0, 0, 0); }
int ioctl(int fd, int req, ...) {
    va_list a;
    void *p;
    va_start(a, req);
    p = va_arg(a, void *);
    va_end(a);
    return call(29, fd, req, (long)p, 0);
}
int fcntl(int fd, int cmd, ...) {
    va_list a;
    long v = 0;
    if (cmd != F_GETFL && cmd != F_GETFD) {
        va_start(a, cmd);
        v = va_arg(a, int);
        va_end(a);
    }
    return call(47, fd, cmd, v, 0);
}
int flock(int fd, int op) { return call(60, fd, op, 0, 0); }
int getgroups(int n, gid_t *p) { return call(74, n, (long)p, 0, 0); }
int _statfs(const char *p, uint8_t *b) {
    return call(22, (long)p, (long)b, 0, 0);
}
int _uname(struct _uzisysinfoblk *p, int len) {
    return call(54, (long)p, len, 0, 0);
}
int system(const char *s) {
    pid_t p;
    int status;
    char *a[4];
    if (!s)
        return access("/bin/sh", X_OK) == 0;
    fflush(NULL);
    p = fork();
    if (p < 0)
        return -1;
    if (!p) {
        a[0] = "sh";
        a[1] = "-c";
        a[2] = (char *)s;
        a[3] = NULL;
        execv("/bin/sh", a);
        _exit(127);
    }
    while (waitpid(p, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    return status;
}
int _time(__ktime_t *p, uint16_t clock) {
    return call(27, (long)p, clock, 0, 0);
}
int _stime(const __ktime_t *p, uint16_t clock) {
    return call(28, (long)p, clock, 0, 0);
}
int stime(const time_t *p) {
    __ktime_t t;
    if(!p){errno=EFAULT;return -1;}
    t.low=*p;t.high=0;
    return _stime(&t,0);
}

int raise(int sig) { return kill(getpid(), sig); }

int tcgetpgrp(int fd) {
    unsigned group;
    if (ioctl(fd, 12, &group) < 0) return -1;
    return group;
}
int tcsetpgrp(int fd, int group) {
    unsigned g = group;
    return ioctl(fd, 13, &g);
}
