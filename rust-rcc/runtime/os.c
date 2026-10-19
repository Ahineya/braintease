/* Carrier-only OS boundary. Rust strings, slices and integers in memory never
 * cross as C structures. Paths arrive as octets plus a length, without a NUL. */
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#define RUSTCELL_PATH_LIMIT 512
#define RUSTCELL_IO_CHUNK 16384UL
static int rustcell_arg_count;
static char **rustcell_arg_values;

static long rustcell_os_error(void) { return -(long)(errno ? errno : EIO); }

unsigned long rustcell_argc(void) { return (unsigned long)rustcell_arg_count; }
long rustcell_arg(unsigned long index, unsigned char *bytes, unsigned long capacity) {
    const unsigned char *source;
    unsigned long length = 0, i;
    if (index >= (unsigned long)rustcell_arg_count) return -(long)EINVAL;
    source = (const unsigned char *)rustcell_arg_values[index];
    while (source[length]) length++;
    if (!capacity) return (long)length;
    if (capacity < length) return -(long)ERANGE;
    for (i = 0; i < length; i++) bytes[i] = source[i] & 255;
    return (long)length;
}

static int rustcell_path(char *path, const unsigned char *bytes, unsigned long length) {
    unsigned long i;
    if (!length) { errno = ENOENT; return -1; }
    if (length >= RUSTCELL_PATH_LIMIT) { errno = ENAMETOOLONG; return -1; }
    for (i = 0; i < length; i++) {
        if (!bytes[i]) { errno = EINVAL; return -1; }
        path[i] = bytes[i] & 255;
    }
    path[length] = 0;
    return 0;
}

/* Stable bridge bits: read=1, write=2, create=4, truncate=8, append=16,
 * create-new=32. Translate here instead of exposing guest fcntl constants. */
long rustcell_open(const unsigned char *bytes, unsigned long length, unsigned short options) {
    char path[RUSTCELL_PATH_LIMIT];
    int flags, fd;
    unsigned access = options & 3;
    if ((options & ~63U) || !access || ((options & 60) && !(options & 2))) return -(long)EINVAL;
    if (rustcell_path(path, bytes, length) < 0) return rustcell_os_error();
    flags = access == 3 ? O_RDWR : (access == 2 ? O_WRONLY : O_RDONLY);
    if (options & 4) flags |= O_CREAT;
    if (options & 8) flags |= O_TRUNC;
    if (options & 16) flags |= O_APPEND;
    if (options & 32) flags |= O_CREAT | O_EXCL;
    do { fd = open(path, flags, 0666); } while (fd < 0 && errno == EINTR);
    return fd < 0 ? rustcell_os_error() : (long)fd;
}

long rustcell_fd_read(short fd, unsigned char *bytes, unsigned long length) {
    long count, i;
    if (length > RUSTCELL_IO_CHUNK) length = RUSTCELL_IO_CHUNK;
    do { count = (long)read(fd, bytes, length); } while (count < 0 && errno == EINTR);
    if (count < 0) return rustcell_os_error();
    for (i = 0; i < count; i++) bytes[i] &= 255;
    return count;
}

long rustcell_fd_write(short fd, const unsigned char *bytes, unsigned long length) {
    long count;
    if (length > RUSTCELL_IO_CHUNK) length = RUSTCELL_IO_CHUNK;
    do { count = (long)write(fd, bytes, length); } while (count < 0 && errno == EINTR);
    return count < 0 ? rustcell_os_error() : count;
}

long rustcell_seek(short fd, long offset, unsigned short origin) {
    long result;
    int whence;
    if (origin > 2) return -(long)EINVAL;
    whence = origin == 0 ? SEEK_SET : (origin == 1 ? SEEK_CUR : SEEK_END);
    do { result = (long)lseek(fd, offset, whence); } while (result < 0 && errno == EINTR);
    return result < 0 ? rustcell_os_error() : result;
}

long rustcell_close(short fd) {
    /* Do not retry close: an interrupted close may already release the fd. */
    return close(fd) < 0 ? rustcell_os_error() : 0;
}

long rustcell_unlink(const unsigned char *bytes, unsigned long length) {
    char path[RUSTCELL_PATH_LIMIT];
    if (rustcell_path(path, bytes, length) < 0) return rustcell_os_error();
    return unlink(path) < 0 ? rustcell_os_error() : 0;
}
