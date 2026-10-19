/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 2026-10-19: buffered records and logical directory cursors; public ABI retained. */
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
extern long call(unsigned, long, long, long, long);
DIR *fdopendir_r(DIR *d, int fd) {
    memset(d, 0, sizeof(*d));
    d->dd_fd = fd;
    d->_priv.de.d_off = lseek(fd, 0, SEEK_CUR);
    if (d->_priv.de.d_off < 0) return NULL;
    return d;
}
DIR *opendir_r(DIR *d, const char *p) {
    struct stat s;
    int fd = open(p, 0);
    if (fd < 0)
        return NULL;
    if (fstat(fd, &s) < 0 || !S_ISDIR(s.st_mode)) {
        close(fd);
        errno = ENOTDIR;
        return NULL;
    }
    if (!fdopendir_r(d, fd)) { close(fd); return NULL; }
    return d;
}
DIR *opendir(const char *p) {
    DIR *d = malloc(sizeof(*d));
    if (!d) {
        errno = ENOMEM;
        return NULL;
    }
    if (!opendir_r(d, p)) {
        free(d);
        return NULL;
    }
    return d;
}
DIR *fdopendir(int fd) {
    DIR *d = malloc(sizeof(*d));
    if (!d) {
        errno = ENOMEM;
        return NULL;
    }
    if (!fdopendir_r(d, fd)) { free(d); return NULL; }
    return d;
}
int closedir_r(DIR *d) { return close(d->dd_fd); }
int closedir(DIR *d) {
    int r = closedir_r(d);
    free(d);
    return r;
}
/* Fetch whole records in batches. de.d_off is the caller's logical cursor;
 * the underlying descriptor may already point past the buffered entries. */
struct dirent *readdir(DIR *d) {
    struct __dirent raw;
    long n;
    for (;;) {
        if (d->_priv.next == d->_priv.last) {
            n = call(24, d->dd_fd, (long)d->_priv.buf, sizeof(d->_priv.buf), 0);
            if (n <= 0) return NULL;
            if (n % sizeof(raw)) { errno = EIO; return NULL; }
            d->_priv.next = 0;
            d->_priv.last = n / sizeof(raw);
        }
        memcpy(&raw, d->_priv.buf + d->_priv.next * sizeof(raw), sizeof(raw));
        ++d->_priv.next;
        d->_priv.de.d_off += sizeof(raw);
        if (raw.d_ino) break;
    }
    d->_priv.de.d_ino = raw.d_ino;
    d->_priv.de.d_reclen = sizeof(struct dirent);
    memcpy(d->_priv.de.d_name, raw.d_name, 30);
    d->_priv.de.d_name[30] = 0;
    return &d->_priv.de;
}
void rewinddir(DIR *d) { seekdir(d, 0); }
void seekdir(DIR *d, off_t n) {
    off_t at = lseek(d->dd_fd, n, SEEK_SET);
    if (at >= 0) {
        d->_priv.next = d->_priv.last = 0;
        d->_priv.de.d_off = at;
    }
}
off_t telldir(DIR *d) { return d->_priv.de.d_off; }
