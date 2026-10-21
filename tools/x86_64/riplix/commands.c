/* SPDX-License-Identifier: GPL-2.0-only */
#include "user.h"
static void require(int condition, const char *message) {
    if (!condition) { fx_puts("PIPE CHECK FAILED: "); fx_puts(message); fx_puts("\n"); fx_exit(1); }
}
void riplix_cat_entry(unsigned long argument) {
    struct fx_arguments *args = (void *)argument;
    char buffer[128]; long n;
    if (args->argc != 2) { fx_puts("usage: cat PATH\n"); fx_exit(2); }
    int fd = fx_open(args->argv[1], O_RDONLY, 0);
    if (fd < 0) { fx_error(args->argv[1]); fx_exit(1); }
    while ((n = fx_read(fd, buffer, sizeof(buffer))) > 0) {
        long offset = 0;
        while (offset < n) {
            long wrote = fx_write(1, buffer + offset, n - offset);
            if (wrote <= 0) fx_exit(1);
            offset += wrote;
        }
    }
    fx_close(fd); fx_exit(n < 0 ? 1 : 0);
}
void riplix_ls_entry(unsigned long argument) {
    struct fx_arguments *args = (void *)argument;
    char entry[DIR_LEN], name[FILENAME_LEN + 1]; long n;
    const char *path = args->argc > 1 ? args->argv[1] : ".";
    int fd = fx_open(path, O_RDONLY, 0);
    if (fd < 0) { fx_error(path); fx_exit(1); }
    while ((n = fx_read(fd, entry, sizeof(entry))) == sizeof(entry)) if (entry[0]) {
        memcpy(name, entry + 1, FILENAME_LEN); name[FILENAME_LEN] = 0;
        fx_puts(name); fx_puts("\n");
    }
    fx_close(fd); fx_exit(n < 0 ? 1 : 0);
}
void riplix_pipecheck_entry(unsigned long argument) {
    int descriptors[2], status, child; unsigned position = 0, i;
    char buffer[193]; long n;
    used(argument);
    require(fx_call(40, (unsigned long)descriptors, 0, 0) == 0, "pipe");
    child = fx_fork(); require(child >= 0, "fork");
    if (!child) {
        fx_close(descriptors[0]);
        while (position < 20000U) {
            unsigned count = 20000U - position;
            if (count > sizeof(buffer)) count = sizeof(buffer);
            for (i = 0; i < count; ++i) buffer[i] = (position + i) % 251;
            n = fx_write(descriptors[1], buffer, count); require(n > 0, "write"); position += n;
        }
        fx_close(descriptors[1]); fx_exit(37);
    }
    fx_close(descriptors[1]);
    while ((n = fx_read(descriptors[0], buffer, sizeof(buffer))) > 0)
        for (i = 0; i < (unsigned)n; ++i) require(buffer[i] == position++ % 251, "contents across ring wrap");
    require(n == 0 && position == 20000U, "length and EOF"); fx_close(descriptors[0]);
    require(fx_wait(child, &status) == child && status == (37 << 8), "wait");
    fx_puts("Riplix: pipe checks passed (20,000 cells, blocking, wrap, EOF and wait)\n"); fx_exit(0);
}
