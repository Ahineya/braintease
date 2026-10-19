/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "include/fde_skin.h"

int fde_slice(const struct fde_skin *s, unsigned width, unsigned height,
              struct fde_patch patches[9])
{
    unsigned sx[4], sy[4], dx[4], dy[4], x, y, n = 0;
    struct fde_patch *p;
    if (!s || !patches || !s->width || !s->height || !width || !height ||
        s->width > 32767 || s->height > 32767 || width > 32767 || height > 32767 ||
        s->centre > 1 || (unsigned long)s->left + s->right >= s->width ||
        (unsigned long)s->top + s->bottom >= s->height ||
        (unsigned long)s->left + s->right > width ||
        (unsigned long)s->top + s->bottom > height)
        return -1;

    sx[0] = sy[0] = dx[0] = dy[0] = 0;
    sx[1] = dx[1] = s->left;
    sy[1] = dy[1] = s->top;
    sx[2] = s->width - s->right; sx[3] = s->width;
    sy[2] = s->height - s->bottom; sy[3] = s->height;
    dx[2] = width - s->right; dx[3] = width;
    dy[2] = height - s->bottom; dy[3] = height;
    for (y = 0; y < 3; ++y) {
        for (x = 0; x < 3; ++x) {
            if ((!s->centre && x == 1 && y == 1) ||
                dx[x] == dx[x + 1] || dy[y] == dy[y + 1])
                continue;
            p = &patches[n++];
            p->sx = sx[x]; p->sy = sy[y];
            p->sw = sx[x + 1] - sx[x]; p->sh = sy[y + 1] - sy[y];
            p->dx = dx[x]; p->dy = dy[y];
            p->dw = dx[x + 1] - dx[x]; p->dh = dy[y + 1] - dy[y];
        }
    }
    return n;
}
