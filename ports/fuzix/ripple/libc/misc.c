/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#undef isascii
int isascii(int c) { return (unsigned)c < 128; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isalpha(int c) { return isupper(c) || islower(c); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isblank(int c) { return c == ' ' || c == '\t'; }
int iscntrl(int c) { return (unsigned)c < 32 || c == 127; }
int isprint(int c) { return c >= 32 && c < 127; }
int isgraph(int c) { return c > 32 && c < 127; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int isxdigit(int c) {
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
int toupper(int c) { return islower(c) ? c - 'a' + 'A' : c; }
int tolower(int c) { return isupper(c) ? c - 'A' + 'a' : c; }
char *strerror(int e) {
    switch (e) {
    case 0:
        return "Success";
    case EPERM:
        return "Operation not permitted";
    case ENOENT:
        return "No such file or directory";
    case ESRCH:
        return "No such process";
    case EINTR:
        return "Interrupted system call";
    case EIO:
        return "Input/output error";
    case EBADF:
        return "Bad file descriptor";
    case ECHILD:
        return "No child processes";
    case EAGAIN:
        return "Resource temporarily unavailable";
    case ENOMEM:
        return "Out of memory";
    case EFAULT:
        return "Bad address";
    case EACCES:
        return "Permission denied";
    case EEXIST:
        return "File exists";
    case EXDEV:
        return "Cross-device link";
    case ENOTDIR:
        return "Not a directory";
    case EISDIR:
        return "Is a directory";
    case EINVAL:
        return "Invalid argument";
    case EMFILE:
        return "Too many open files";
    case ENOSPC:
        return "No space left on device";
    case ESPIPE:
        return "Illegal seek";
    case EROFS:
        return "Read-only filesystem";
    case ENOTEMPTY:
        return "Directory not empty";
    case ENOEXEC:
        return "Exec format error";
    case EFBIG: return "File too large";
    case ENAMETOOLONG: return "File name too long";
    case EAFNOSUPPORT: return "Address family not supported";
    case EALREADY: return "Operation already in progress";
    case EADDRINUSE: return "Address already in use";
    case EADDRNOTAVAIL: return "Address not available";
    case EOPNOTSUPP: return "Operation not supported";
    case ECONNRESET: return "Connection reset by peer";
    case ENETDOWN: return "Network is down";
    case EMSGSIZE: return "Message too long";
    case ETIMEDOUT: return "Connection timed out";
    case ECONNREFUSED: return "Connection refused";
    case EHOSTUNREACH: return "No route to host";
    case EHOSTDOWN: return "Host is down";
    case ENETUNREACH: return "Network is unreachable";
    case ENOTCONN: return "Socket is not connected";
    case EINPROGRESS: return "Operation now in progress";
    case ESHUTDOWN: return "Socket is shut down";
    case EISCONN: return "Socket is already connected";
    case EDESTADDRREQ: return "Destination address required";
    case ENOBUFS: return "No buffer space available";
    case ELOOP: return "Too many symbolic links";
    case EOVERFLOW: return "Value too large for caller ABI";
    case EPROTONOSUPPORT: return "Protocol not supported";
    default:
        return "System error";
    }
}
void perror(const char *p) {
    int e = errno;
    if (p && *p)
        fprintf(stderr, "%s: ", p);
    fprintf(stderr, "%s\n", strerror(e));
    errno = e;
}
char *getenv(char *name) {
    size_t n = strlen(name);
    char **p = environ;
    if (p)
        for (; *p; p++)
            if (!strncmp(*p, name, n) && (*p)[n] == '=')
                return *p + n + 1;
    return NULL;
}
const char *_ultoa(unsigned long n) {
    static char b[34];
    sprintf(b, "%lu", n);
    return b;
}
const char *_ltoa(long n) {
    static char b[34];
    sprintf(b, "%ld", n);
    return b;
}
const char *_uitoa(unsigned n) { return _ultoa(n); }
const char *_itoa(int n) { return _ltoa(n); }
char *ltoa(long n, char *b, int base) {
    char t[34];
    unsigned i = 0, j = 0;
    unsigned long v = n;
    if (base < 2 || base > 36) {
        b[0] = 0;
        return b;
    }
    if (n < 0 && base == 10) {
        b[j++] = '-';
        v = 0UL - v;
    }
    do {
        unsigned d = v % base;
        t[i++] = d < 10 ? '0' + d : 'a' + d - 10;
        v /= base;
    } while (v);
    while (i)
        b[j++] = t[--i];
    b[j] = 0;
    return b;
}
char *ultoa(unsigned long n, char *b, int base) {
    char t[34];
    unsigned i = 0, j = 0;
    if (base < 2 || base > 36) {
        b[0] = 0;
        return b;
    }
    do {
        unsigned d = n % base;
        t[i++] = d < 10 ? '0' + d : 'a' + d - 10;
        n /= base;
    } while (n);
    while (i)
        b[j++] = t[--i];
    b[j] = 0;
    return b;
}
char *setlocale(int c, const char *s) {
    (void)c;
    if (!s || !*s || !strcmp(s, "C") || !strcmp(s, "POSIX"))
        return "C";
    return NULL;
}
static void diagnostic(int with_errno, const char *fmt, va_list a) {
    int e = errno;
    if (_argv && _argv[0])
        fprintf(stderr, "%s: ", _argv[0]);
    if (fmt)
        vfprintf(stderr, fmt, a);
    if (with_errno)
        fprintf(stderr, "%s%s", fmt ? ": " : "", strerror(e));
    fputc('\n', stderr);
    errno = e;
}
void warn(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    diagnostic(1, fmt, a);
    va_end(a);
}
void warnx(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    diagnostic(0, fmt, a);
    va_end(a);
}
void err(int status, const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    diagnostic(1, fmt, a);
    va_end(a);
    exit(status);
}
void errx(int status, const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    diagnostic(0, fmt, a);
    va_end(a);
    exit(status);
}

void regerror(char *s) {
    fprintf(stderr, "regexp: %s\n", s);
    exit(2);
}
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
int statvfs(const char *path, struct statvfs *v) {
    struct {
        struct _uzifilesys fs;
        unsigned flags;
    } k;
    if (_statfs(path, (uint8_t *)&k) < 0)
        return -1;
    if (k.fs.s_mounted != 0x7254) {
        errno = EINVAL;
        return -1;
    }
    v->f_bsize = v->f_frsize =
        2048; /* C16 V3 physical octets, as expected by upstream df. */
    v->f_blocks = k.fs.s_fsize - k.fs.s_isize;
    v->f_bfree = v->f_bavail = k.fs.s_tfree;
    v->f_files = (k.fs.s_isize - 2) * 16;
    v->f_ffree = v->f_favail = k.fs.s_tinode;
    v->f_fsid = 0;
    v->f_namemax = 30;
    v->f_flag = (k.flags & MS_RDONLY ? ST_RDONLY : 0) |
                (k.flags & MS_NOSUID ? ST_NOSUID : 0) |
                (k.flags & MS_NOEXEC ? ST_NOEXEC : 0);
    return 0;
}
int uname(struct utsname *s) {
    strcpy(s->sysname, "FUZIX");
    strcpy(s->nodename, "ripple");
    strcpy(s->release, "0.5");
    strcpy(s->version, "RCC");
    strcpy(s->machine, "Ripple");
    return 0;
}
char *stpcpy(char *dest, const char *src) {
    while ((*dest = *src++))
        dest++;
    return dest;
}
/* Atomic exclusive creation; generated names are not an access-control boundary. */
static int temp_object(char *pattern, int directory) {
    static unsigned long serial;
    size_t len = strlen(pattern), start = len;
    unsigned attempt;
    while (start && pattern[start - 1] == 'X') start--;
    if (len - start < 6 || len >= 512) {
        errno = EINVAL;
        return -1;
    }
    for (attempt = 0; attempt < 256; attempt++) {
        unsigned long n = ((unsigned long)getpid() << 16) ^ serial++;
        size_t i = len;
        int result;
        while (i > start) {
            unsigned digit = n % 36;
            pattern[--i] = digit < 10 ? '0' + digit : 'a' + digit - 10;
            n /= 36;
        }
        result = directory ? mkdir(pattern, 0700) :
                 open(pattern, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (result >= 0 || errno != EEXIST) return result;
    }
    errno = EEXIST;
    return -1;
}
int mkstemp(char *pattern) { return temp_object(pattern, 0); }
char *mkdtemp(char *pattern) {
    return temp_object(pattern, 1) < 0 ? NULL : pattern;
}
const char *strsignal(int sig) {
    static const char *names[] = {
        "Unknown signal", "Hangup", "Interrupt", "Quit", "Illegal instruction",
        "Trace trap", "Aborted", "Bus error", "Arithmetic exception", "Killed",
        "User signal 1", "Segmentation fault", "User signal 2", "Broken pipe",
        "Alarm", "Terminated"
    };
    return sig >= 0 && sig < 16 ? names[sig] : names[0];
}
void __errput(const char *text) {
    fputs(text, stderr);
    fflush(stderr);
}
void __assert(const char *expression, const char *file, int line) {
    fprintf(stderr, "%s:%d: assertion failed: %s\n", file, line, expression);
    fflush(stderr);
    abort();
}
