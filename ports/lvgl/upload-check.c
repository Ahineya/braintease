/* SPDX-License-Identifier: MIT */
/* Exercise the new optional upload through the real Unix ownership boundary. */
#include <gx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <errno.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "UPLOAD_CHECK line %u errno=%d\n", __LINE__, errno);                   \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
int main(void) {
    struct gx_info info;
    struct gx_window w;
    struct gx_upload u;
    struct gx_handle h;
    struct gx_formats formats;
    struct gx_fill_rgb888 fill;
    struct gx_copy copy;
    unsigned long gap;
    uint16_t *allocation, *data;
    int fd, pid, status;
    static const uint16_t source[] = {0, 0, 255, 0, 255, 0, 999, 999, 255, 0, 0, 255, 255, 255};
    fd = open("/dev/gfx", O_RDWR);
    CHECK(fd >= 0);
    CHECK(ioctl(fd, GX_INFO, &info) == 0 && (info.flags & GX_CAP_BGR888_UPLOAD));
    memset(&w, 0, sizeof(w));
    w.version = GX_VERSION;
    w.width = 4;
    w.height = 3;
    w.format = GX_RGB565;
    strcpy(w.title, "BGR888 upload check");
    CHECK(ioctl(fd, GX_CREATE, &w) == 0);
    allocation = malloc(70000UL * sizeof(*allocation));
    CHECK(allocation);
    gap = 64000UL - ((unsigned long)allocation & 65535UL) - 2;
    data = allocation + gap;
    memcpy(data, source, sizeof(source));
    memset(&u, 0, sizeof(u));
    u.version = GX_VERSION;
    u.surface = w.surface;
    u.x = u.y = 1;
    u.width = u.height = 2;
    u.stride = 8;
    u.pixels = data;
    CHECK(ioctl(fd, GX_UPLOAD_BGR888, &u) == 0);
    u.stride = 5;
    CHECK(ioctl(fd, GX_UPLOAD_BGR888, &u) < 0 && errno == EINVAL);
    u.stride = 8;
    u.pixels = NULL;
    CHECK(ioctl(fd, GX_UPLOAD_BGR888, &u) < 0 && errno == EFAULT);
    u.pixels = data;
    u.x = 4;
    CHECK(ioctl(fd, GX_UPLOAD_BGR888, &u) < 0 && errno == EINVAL);
    u.x = 1;
    pid = fork();
    CHECK(pid >= 0);
    if (!pid)
        _exit(ioctl(fd, GX_UPLOAD_BGR888, &u) < 0 && errno == EPERM ? 0 : 1);
    CHECK(waitpid(pid, &status, 0) == pid && status == 0);
    h.version = GX_VERSION;
    h.handle = w.window;
    CHECK(ioctl(fd, GX_PRESENT, &h) == 0);
    CHECK(ioctl(fd, GX_DESTROY, &h) == 0);
    CHECK(ioctl(fd, GX_UPLOAD_BGR888, &u) < 0 && errno == EINVAL);
    memset(&formats, 0, sizeof(formats));
    formats.version = GX_VERSION + 1;
    CHECK(ioctl(fd, GX_FORMATS, &formats) < 0 && errno == EINVAL);
    formats.version = GX_VERSION;
    CHECK(ioctl(fd, GX_FORMATS, &formats) == 0 && formats.preferred == GX_XRGB8888 &&
          (formats.formats & GX_FORMAT_RGB565) && (formats.formats & GX_FORMAT_XRGB8888));
    w.format = 3;
    CHECK(ioctl(fd, GX_CREATE, &w) < 0 && errno == EINVAL);
    w.format = GX_XRGB8888;
    CHECK(ioctl(fd, GX_CREATE, &w) == 0);
    memset(&fill, 0, sizeof(fill));
    fill.version = GX_VERSION;
    fill.surface = w.surface;
    fill.width = 4;
    fill.height = 3;
    fill.colour = 0x1000000UL;
    CHECK(ioctl(fd, GX_FILL_RGB888, &fill) < 0 && errno == EINVAL);
    fill.colour = 0x222324UL;
    CHECK(ioctl(fd, GX_FILL_RGB888, &fill) == 0);
    {
        static const uint16_t precise[] = {3, 2, 1, 6, 5, 4, 999, 999, 9, 8, 7, 12, 11, 10};
        memcpy(data, precise, sizeof(precise));
    }
    u.surface = w.surface;
    CHECK(ioctl(fd, GX_UPLOAD_BGR888, &u) == 0);
    memset(&copy, 0, sizeof(copy));
    copy.version = GX_VERSION;
    copy.surface = w.surface;
    copy.sx = copy.sy = 1;
    copy.width = copy.height = 2;
    CHECK(ioctl(fd, GX_COPY, &copy) == 0);
    pid = fork();
    CHECK(pid >= 0);
    if (!pid)
        _exit(ioctl(fd, GX_FILL_RGB888, &fill) < 0 && errno == EPERM ? 0 : 1);
    CHECK(waitpid(pid, &status, 0) == pid && status == 0);
    h.handle = w.window;
    CHECK(ioctl(fd, GX_PRESENT, &h) == 0);
    CHECK(ioctl(fd, GX_DESTROY, &h) == 0);
    CHECK(ioctl(fd, GX_FILL_RGB888, &fill) < 0 && errno == EINVAL);
    free(allocation);
    close(fd);
    puts("UPLOAD_CHECK passed RGB565/full-colour, padded cross-bank source, bounds, ownership and "
         "teardown");
    return 0;
}
