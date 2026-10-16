/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
extern long call(unsigned, long, long, long, long);
DIR *fdopendir_r(DIR *d, int fd) {
    memset(d, 0, sizeof(*d));
    d->dd_fd = fd;
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
    return fdopendir_r(d, fd);
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
    return fdopendir_r(d, fd);
}
int closedir_r(DIR *d) { return close(d->dd_fd); }
int closedir(DIR *d) {
    int r = closedir_r(d);
    free(d);
    return r;
}
struct dirent *readdir(DIR *d) {
    struct __dirent raw;
    long n;
    do {
        n = call(24, d->dd_fd, (long)&raw, sizeof(raw), 0);
        if (n <= 0)
            return NULL;
        if (n != sizeof(raw)) {
            errno = EIO;
            return NULL;
        }
    } while (!raw.d_ino);
    d->_priv.de.d_ino = raw.d_ino;
    d->_priv.de.d_off = lseek(d->dd_fd, 0, SEEK_CUR);
    d->_priv.de.d_reclen = sizeof(struct dirent);
    memcpy(d->_priv.de.d_name, raw.d_name, 30);
    d->_priv.de.d_name[30] = 0;
    return &d->_priv.de;
}
void rewinddir(DIR *d) { lseek(d->dd_fd, 0, SEEK_SET); }
void seekdir(DIR *d, off_t n) { lseek(d->dd_fd, n, SEEK_SET); }
off_t telldir(DIR *d) { return lseek(d->dd_fd, 0, SEEK_CUR); }
