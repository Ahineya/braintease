/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NXR_CLIPBOARD_WIRE_H
#define NXR_CLIPBOARD_WIRE_H
#include "nx_clipboard.h"
#define NXR_CLIPBOARD 251
#define NXR_CLIPBOARD_CHUNK 512
#define NXR_CLIP_GET 1
#define NXR_CLIP_READ 2
#define NXR_CLIP_SET 3
#define NXR_CLIP_WRITE 4
#define NXR_CLIP_COMMIT 5
#define NXR_CLIP_END 6
struct nxr_clip_req {
    nxReq header;
    unsigned version, op;
    GR_WINDOW_ID window;
    unsigned long length;
    unsigned count;
};
struct nxr_clip_reply {
    unsigned status, count;
    unsigned long length;
    char text[NXR_CLIPBOARD_CHUNK];
};
#endif
