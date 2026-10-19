/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Included in the adapted client to reuse its typed reply/event handling. */
#include "fde_nano.h"
#include "image-wire.h"
static unsigned image_status;
unsigned fde_image_status(void) {return image_status;}
static int image_request(struct nxr_image_req *q, const void *payload, unsigned cells,
                          struct nxr_image_reply *reply) {
    struct nxr_image_req *wire;
    nxReq header;
    LOCK(&nxGlobalLock);
    wire = nxAllocReq(NXR_IMAGE, sizeof(*wire), cells);
    header = wire->header; *wire = *q; wire->header = header;
    wire->version = NXR_IMAGE_VERSION;
    if (cells) memcpy(wire + 1, payload, cells);
    if (TypedReadBlock(reply, sizeof(*reply), NXR_IMAGE) == -1) reply->status = NXR_IMAGE_INVALID;
    image_status = reply->status;
    UNLOCK(&nxGlobalLock);
    return image_status ? -1 : 0;
}
FDE_IMAGE fde_image_create(unsigned width, unsigned height, unsigned flags) {
    struct nxr_image_req q; struct nxr_image_reply reply;
    memset(&q, 0, sizeof(q)); q.op = NXR_IMAGE_CREATE; q.width = width; q.height = height; q.flags = flags;
    return image_request(&q, NULL, 0, &reply) < 0 ? 0 : reply.id;
}
int fde_image_upload(FDE_IMAGE id, unsigned long offset, const unsigned short *data, unsigned count) {
    struct nxr_image_req q; struct nxr_image_reply reply;
    if (!data || !count || count > NXR_IMAGE_CHUNK) {image_status = NXR_IMAGE_INVALID; return -1;}
    memset(&q, 0, sizeof(q)); q.op = NXR_IMAGE_UPLOAD; q.id = id; q.offset = offset; q.count = count;
    return image_request(&q, data, count * sizeof(unsigned short), &reply);
}
int fde_image_destroy(FDE_IMAGE id) {
    struct nxr_image_req q; struct nxr_image_reply reply;
    memset(&q, 0, sizeof(q)); q.op = NXR_IMAGE_DESTROY; q.id = id;
    return image_request(&q, NULL, 0, &reply);
}
int fde_image_draw(FDE_IMAGE id, GR_DRAW_ID target, GR_GC_ID gc,
                   const struct fde_rect *rects, unsigned count, unsigned flags) {
    struct nxr_image_req q; struct nxr_image_reply reply;
    if (!rects || !count || count > 9) {image_status = NXR_IMAGE_INVALID; return -1;}
    memset(&q, 0, sizeof(q)); q.op = NXR_IMAGE_DRAW; q.id = id; q.target = target; q.gc = gc;
    q.count = count; q.flags = flags;
    return image_request(&q, rects, count * sizeof(*rects), &reply);
}
FDE_IMAGE fde_image_load(unsigned width, unsigned height, const unsigned short *pixels, const unsigned short *mask) {
    FDE_IMAGE id;
    unsigned stage, count, saved;
    unsigned long offset = 0, remaining;
    const unsigned short *data;
    if (!pixels && !mask) {image_status = NXR_IMAGE_INVALID; return 0;}
    id = fde_image_create(width, height, mask ? (pixels ? FDE_IMAGE_MASK : FDE_IMAGE_MASK_ONLY) : 0);
    if (!id) return 0;
    for (stage = 0; stage < 2; ++stage) {
        data = stage ? mask : pixels;
        remaining = data ? (unsigned long)(stage ? (width + 15) / 16 : width) * height : 0;
        while (remaining) {
            count = remaining > NXR_IMAGE_CHUNK ? NXR_IMAGE_CHUNK : (unsigned)remaining;
            if (fde_image_upload(id, offset, data, count) < 0) {
                saved = image_status; fde_image_destroy(id); image_status = saved; return 0;
            }
            data += count; offset += count; remaining -= count;
        }
    }
    return id;
}
int fde_skin_paint(FDE_IMAGE id, GR_DRAW_ID target, GR_GC_ID gc, unsigned ax, unsigned ay,
                   const struct fde_skin *skin, int x, int y, unsigned width, unsigned height) {
    struct fde_patch patches[9]; struct fde_rect rects[9];
    int n = fde_slice(skin, width, height, patches), i;
    long dx, dy;
    if (n < 0) {image_status = NXR_IMAGE_INVALID; return -1;}
    if (!n) return 0;
    for (i = 0; i < n; ++i) {
        dx = (long)x + patches[i].dx; dy = (long)y + patches[i].dy;
        if (dx < -32768L || dx > 32767 || dy < -32768L || dy > 32767 ||
            (unsigned long)ax + patches[i].sx > 511 || (unsigned long)ay + patches[i].sy > 511) {
            image_status = NXR_IMAGE_INVALID; return -1;
        }
        rects[i].x = dx; rects[i].y = dy; rects[i].sx = ax + patches[i].sx; rects[i].sy = ay + patches[i].sy;
        rects[i].sw = patches[i].sw; rects[i].sh = patches[i].sh;
        rects[i].width = patches[i].dw; rects[i].height = patches[i].dh;
    }
    return fde_image_draw(id, target, gc, rects, n, 0);
}
#include "../../fusede/image-file.inc"
