/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Private same-C16-ABI extension. Include nano-X.h and nxproto.h first.
 * Payload lengths count C16 cells, never host octets. There are no pointers. */
#ifndef NXR_IMAGE_WIRE_H
#define NXR_IMAGE_WIRE_H
#include "fde_raster.h"
#define NXR_IMAGE 253
#define NXR_IMAGE_VERSION 1
#define NXR_IMAGE_CREATE 1
#define NXR_IMAGE_UPLOAD 2
#define NXR_IMAGE_DRAW 3
#define NXR_IMAGE_DESTROY 4
#define NXR_IMAGE_MASK 1
#define NXR_IMAGE_MASK_ONLY 2
#define NXR_IMAGE_ALPHA 4
#define NXR_IMAGE_TINT 1
#define NXR_IMAGE_OK 0
#define NXR_IMAGE_INVALID 1
#define NXR_IMAGE_QUOTA 2
#define NXR_IMAGE_NOMEM 3
#define NXR_IMAGE_INCOMPLETE 4
#define NXR_IMAGE_CHUNK 2048
struct nxr_image_req {
    nxReq header;
    unsigned version, op;
    unsigned long id;
    GR_DRAW_ID target;
    GR_GC_ID gc;
    unsigned width, height, flags, count;
    unsigned long offset;
};
struct nxr_image_reply {unsigned long id; unsigned status;};
#endif
