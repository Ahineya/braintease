/* SPDX-License-Identifier: MIT */
#ifndef GRAPHITE_FONT_DATA_H
#define GRAPHITE_FONT_DATA_H
#include "lvgl.h"

/* C16 stores two original A8 octets in each immutable uint16_t static cell,
 * then expands each face once to an ordinary, stable, heap-owned A8 array.
 * Open before lv_init; close only after lv_deinit and all external glyph/mask
 * caches have been released. Both calls are repeat-safe; failed open releases
 * every face. The byte-addressed host reference keeps the original assets. */
int gx_font_data_open(void);
void gx_font_data_close(void);

/* Internal interface used only by the generated packed-font translation units.
 * Font descriptors and all pointers remain inside the application; no GX ABI
 * or host graphics service participates in decoding. */
int gx_font_data_expand(lv_font_fmt_txt_dsc_t *descriptor,
                         const uint16_t *packed, unsigned long length);
void gx_font_data_release(lv_font_fmt_txt_dsc_t *descriptor);
#endif
