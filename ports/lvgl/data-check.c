/* SPDX-License-Identifier: MIT */
/* Test-only: inspect the real guest clipboard and verify source files stay intact. */
#include <gx.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
int main(int argc, char **argv) {
    char buffer[257];
    uint32_t hash = 2166136261UL, size = 0;
    int fd, n;
    unsigned i;
    if (argc != 2)
        return 2;
    if (!strcmp(argv[1], "clipboard")) {
        struct gx_window w;
        struct gx_clipboard c;
        char *text = malloc(GX_CLIPBOARD_MAX);
        if (!text || (fd = open("/dev/gfx", O_RDWR)) < 0)
            return 1;
        memset(&w, 0, sizeof(w));
        w.version = GX_VERSION;
        w.width = w.height = 1;
        w.format = GX_RGB565;
        strcpy(w.title, "Clipboard test");
        if (ioctl(fd, GX_CREATE_HIDDEN, &w))
            return 1;
        memset(&c, 0, sizeof(c));
        c.version = GX_VERSION;
        c.window = w.window;
        c.text = text;
        c.capacity = GX_CLIPBOARD_MAX;
        if (ioctl(fd, GX_CLIPBOARD_READ, &c))
            return 1;
        for (size = 0; size < c.length; size++)
            hash = (hash ^ (unsigned char)text[size]) * 16777619UL;
        close(fd);
        free(text);
    } else {
        fd = open(argv[1], O_RDONLY);
        if (fd < 0)
            return 1;
        while ((n = read(fd, buffer, sizeof(buffer))) > 0)
            for (i = 0; i < (unsigned)n; i++) {
                hash = (hash ^ (unsigned char)buffer[i]) * 16777619UL;
                size++;
            }
        close(fd);
        if (n < 0)
            return 1;
    }
    printf("DATA_CHECK %lu %08lx\n", (unsigned long)size, (unsigned long)hash);
    return 0;
}
