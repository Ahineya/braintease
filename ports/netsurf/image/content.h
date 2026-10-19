/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NETSURF_C16_IMAGE_H
#define NETSURF_C16_IMAGE_H
#include "utils/errors.h"
#include "../../../libraries/image/image.h"
nserror netsurf_images_register(void);
/* Optional frontend resource invalidation before decoded image storage dies. */
void netsurf_images_dispose(void (*callback)(const RiImage *));
#endif
