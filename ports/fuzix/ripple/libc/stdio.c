/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static FILE streams[16];
FILE *stdin = streams, *stdout = streams + 1, *stderr = streams + 2;
#define READING 1
#define WRITING 2
#define ACTIVE_WRITE 4
void ripple_stdio_init(void) {
    int i;
    for (i = 0; i < 3; i++) {
        streams[i].fd = i;
        streams[i].used = 1;
        streams[i].flags = i ? WRITING : READING;
        streams[i].pushed = EOF;
        streams[i].buffering = i == 2 ? _IONBF : (i == 1 ? _IOLBF : _IOFBF);
    }
}
int fflush(FILE *f) {
    size_t off = 0;
    ssize_t n;
    int i, r = 0;
    if (!f) {
        for (i = 0; i < 16; i++)
            if (streams[i].used && (streams[i].flags & ACTIVE_WRITE))
                if (fflush(streams + i) < 0)
                    r = EOF;
        return r;
    }
    if (!f->used) {
        errno = EBADF;
        return EOF;
    }
    if (f->flags & ACTIVE_WRITE) {
        while (off < f->len) {
            n = write(f->fd, f->buf + off, f->len - off);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) {
                if (!n)
                    errno = EIO;
                memmove(f->buf, f->buf + off, f->len - off);
                f->len -= off;
                f->err = 1;
                return EOF;
            }
            off += n;
        }
    } else if (f->pos < f->len || f->pushed != EOF) {
        long back = (long)(f->len - f->pos) + (f->pushed != EOF);
        if (lseek(f->fd, -back, SEEK_CUR) < 0)
            return EOF;
    }
    f->pos = f->len = 0;
    f->pushed = EOF;
    f->flags &= ~ACTIVE_WRITE;
    return 0;
}
int fclose(FILE *f) {
    int r;
    if (!f->used) {
        errno = EBADF;
        return EOF;
    }
    /* Closing a read stream must not seek pipes/terminals backwards. */
    r = (f->flags & ACTIVE_WRITE) ? fflush(f) : 0;
    if (close(f->fd) < 0)
        r = EOF;
    f->used = 0;
    return r;
}
static int mode_flags(const char *m) {
    int f;
    if (*m == 'r')
        f = O_RDONLY;
    else if (*m == 'w')
        f = O_WRONLY | O_CREAT | O_TRUNC;
    else if (*m == 'a')
        f = O_WRONLY | O_CREAT | O_APPEND;
    else {
        errno = EINVAL;
        return -1;
    }
    if (strchr(m, '+'))
        f = (f & ~O_ACCMODE) | O_RDWR;
    return f;
}
static FILE *stream_init(FILE *f, int fd, int mode) {
    memset(f, 0, sizeof(*f));
    f->fd = fd;
    f->used = 1;
    f->pushed = EOF;
    f->flags = (mode & O_ACCMODE) == O_RDWR
                   ? READING | WRITING
                   : ((mode & O_ACCMODE) == O_RDONLY ? READING : WRITING);
    f->buffering = isatty(fd) ? _IOLBF : _IOFBF;
    return f;
}
FILE *fdopen(int fd, const char *m) {
    int i, mode = mode_flags(m);
    if (mode < 0)
        return NULL;
    for (i = 0; i < 16; i++)
        if (!streams[i].used)
            return stream_init(streams + i, fd, mode);
    errno = EMFILE;
    return NULL;
}
FILE *fopen(const char *p, const char *m) {
    int mode = mode_flags(m), fd;
    FILE *f;
    if (mode < 0)
        return NULL;
    fd = open(p, mode, 0666);
    if (fd < 0)
        return NULL;
    f = fdopen(fd, m);
    if (!f)
        close(fd);
    return f;
}
FILE *freopen(const char *p, const char *m, FILE *f) {
    int mode = mode_flags(m), fd;
    if (mode < 0)
        return NULL;
    fclose(f);
    fd = open(p, mode, 0666);
    if (fd < 0)
        return NULL;
    return stream_init(f, fd, mode);
}
FILE *tmpfile(void) {
    char name[] = "/tmp/rccstdioXXXXXX";
    int fd = mkstemp(name), saved;
    FILE *file;
    if (fd < 0) return NULL;
    if (unlink(name) < 0) { saved = errno; close(fd); errno = saved; return NULL; }
    file = fdopen(fd, "w+");
    if (!file) { saved = errno; close(fd); errno = saved; }
    return file;
}
int fgetc(FILE *f) {
    ssize_t n;
    if (!f->used || !(f->flags & READING)) {
        errno = EBADF;
        f->err = 1;
        return EOF;
    }
    if ((f->flags & ACTIVE_WRITE) && fflush(f) < 0)
        return EOF;
    if (f->pushed != EOF) {
        int c = f->pushed;
        f->pushed = EOF;
        return c;
    }
    if (f->pos == f->len) {
        if (f->eof)
            return EOF;
        do {
            n = read(f->fd, f->buf, f->buffering == _IONBF ? 1 : BUFSIZ);
        } while (n < 0 && errno == EINTR);
        if (n <= 0) {
            if (n < 0)
                f->err = 1;
            else
                f->eof = 1;
            return EOF;
        }
        f->pos = 0;
        f->len = n;
    }
    return (unsigned char)f->buf[f->pos++];
}
int fputc(int c, FILE *f) {
    if (!f->used || !(f->flags & WRITING)) {
        errno = EBADF;
        f->err = 1;
        return EOF;
    }
    if (!(f->flags & ACTIVE_WRITE) && fflush(f) < 0) {
        f->err = 1;
        return EOF;
    }
    f->flags |= ACTIVE_WRITE;
    f->buf[f->len++] = c;
    if (f->len == BUFSIZ || f->buffering == _IONBF ||
        (f->buffering == _IOLBF && c == '\n'))
        if (fflush(f) < 0)
            return EOF;
    return (unsigned char)c;
}
int ungetc(int c, FILE *f) {
    if (c == EOF || f->pushed != EOF || !(f->flags & READING))
        return EOF;
    if ((f->flags & ACTIVE_WRITE) && fflush(f) < 0)
        return EOF;
    f->pushed = (unsigned char)c;
    f->eof = 0;
    return f->pushed;
}
char *fgets(char *p, size_t n, FILE *f) {
    size_t i = 0;
    int c;
    if (!n)
        return NULL;
    while (i + 1 < n && (c = fgetc(f)) != EOF) {
        p[i++] = c;
        if (c == '\n')
            break;
    }
    p[i] = 0;
    return i ? p : NULL;
}
int fputs(const char *s, FILE *f) {
    while (*s)
        if (fputc(*s++, f) == EOF)
            return EOF;
    return 0;
}
int puts(const char *s) {
    return fputs(s, stdout) < 0 ? EOF : fputc('\n', stdout);
}
size_t fread(void *p, size_t size, size_t count, FILE *f) {
    size_t i, j;
    char *d = p;
    int c;
    if (!size)
        return 0;
    for (i = 0; i < count; i++)
        for (j = 0; j < size; j++) {
            c = fgetc(f);
            if (c == EOF)
                return i;
            *d++ = c;
        }
    return count;
}
size_t fwrite(const void *p, size_t size, size_t count, FILE *f) {
    size_t i, j;
    const char *s = p;
    if (!size)
        return 0;
    for (i = 0; i < count; i++)
        for (j = 0; j < size; j++)
            if (fputc(*s++, f) == EOF)
                return i;
    return count;
}
long ftell(FILE *f) {
    long n = lseek(f->fd, 0, SEEK_CUR);
    if (n < 0)
        return -1;
    if (f->flags & ACTIVE_WRITE)
        return n + f->len;
    return n - (long)(f->len - f->pos) - (f->pushed != EOF);
}
int fseek(FILE *f, long off, int whence) {
    if (fflush(f) < 0 || lseek(f->fd, off, whence) < 0)
        return -1;
    f->eof = 0;
    return 0;
}
void rewind(FILE *f) {
    fseek(f, 0, SEEK_SET);
    clearerr(f);
}
int fgetpos(FILE *f, fpos_t *p) {
    *p = ftell(f);
    return *p < 0 ? -1 : 0;
}
int fsetpos(FILE *f, const fpos_t *p) { return fseek(f, *p, SEEK_SET); }
int setvbuf(FILE *f, char *b, int mode, size_t n) {
    (void)b;
    (void)n;
    if (mode < _IOFBF || mode > _IONBF || fflush(f) < 0)
        return -1;
    f->buffering = mode;
    return 0;
}
void setbuf(FILE *f, char *b) { setvbuf(f, b, b ? _IOFBF : _IONBF, BUFSIZ); }
