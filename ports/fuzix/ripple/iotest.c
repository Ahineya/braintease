/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "user.h"
static char buffer[257];
static char pattern(unsigned long pos) { return (char)(33 + (pos % 90)); }
static int fail(void) {
    out("iotest FAILED errno=");
    number(ripple_error);
    out("\n");
    return 1;
}
int main(int argc, char **argv, char **envp) {
    int fd;
    unsigned i, n;
    unsigned long pos, length = 77777UL;
    long offset;
    (void)envp;
    if (argc > 1 && !strcmp(argv[1], "create")) {
        fd = open("/iotest.dat", 1 | 256 | 1024, 0644);
        if (fd < 0)
            return fail();
        for (pos = 0; pos < length; pos += n) {
            n = (unsigned)(length - pos > 257 ? 257 : length - pos);
            for (i = 0; i < n; i++)
                buffer[i] = pattern(pos + i);
            if (write(fd, buffer, n) != n)
                return fail();
        }
        close(fd);
        call(11, 0, 0, 0, 0);
        out("iotest created 77777 characters\n");
        return 0;
    }
    fd = open("/iotest.dat", 2, 0);
    if (fd < 0)
        return fail();
    for (pos = 0; pos < length; pos += n) {
        n = (unsigned)(length - pos > 257 ? 257 : length - pos);
        if (read(fd, buffer, n) != n)
            return fail();
        for (i = 0; i < n; i++)
            if (buffer[i] != pattern(pos + i))
                return fail();
    }
    if (read(fd, buffer, 1) != 0)
        return fail();
    offset = 70000;
    if (call(9, fd, (long)&offset, 0, 0) < 0 || read(fd, buffer, 257) != 257)
        return fail();
    for (i = 0; i < 257; i++)
        if (buffer[i] != pattern(70000UL + i))
            return fail();
    offset = 257;
    if (call(67, fd, (long)&offset, 0, 0) < 0)
        return fail();
    offset = 0;
    if (call(9, fd, (long)&offset, 0, 0) < 0 || read(fd, buffer, 257) != 257 ||
        read(fd, buffer, 1) != 0)
        return fail();
    close(fd);
    if (call(6, (long)"/iotest.dat", 0, 0, 0) < 0)
        return fail();
    out("iotest verified, sought, truncated and deleted\n");
    return 0;
}
