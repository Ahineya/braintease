/* SPDX-License-Identifier: MIT */
#include "font-data.h"
#include <limits.h>
#include <stdlib.h>

int gx_font_data_expand(lv_font_fmt_txt_dsc_t *d, const uint16_t *packed,
                         unsigned long length) {
    unsigned long i;
    uint8_t *data;
    if (!d || !packed || !length || length > 1048576UL)
        return -1;
    if (d->glyph_bitmap)
        return 0;
    data = malloc(length * sizeof(*data));
    if (!data)
        return -1;
    for (i = 0; i < length; i++)
        data[i] = (packed[i / 2] >> ((i & 1) * 8)) & 255;
    d->glyph_bitmap = data;
    return 0;
}
void gx_font_data_release(lv_font_fmt_txt_dsc_t *d) {
    if (d && d->glyph_bitmap) {
        free((void *)d->glyph_bitmap);
        d->glyph_bitmap = NULL;
    }
}

#if CHAR_BIT == 16 || defined(GX_FONT_DATA_TEST)
extern int gx_font_data_12_open(void), gx_font_data_14_open(void), gx_font_data_16_open(void);
extern int gx_font_data_24_open(void), gx_font_data_28_open(void), gx_font_data_32_open(void);
extern void gx_font_data_12_close(void), gx_font_data_14_close(void), gx_font_data_16_close(void);
extern void gx_font_data_24_close(void), gx_font_data_28_close(void), gx_font_data_32_close(void);
void gx_font_data_close(void) {
    gx_font_data_32_close();
    gx_font_data_28_close();
    gx_font_data_24_close();
    gx_font_data_16_close();
    gx_font_data_14_close();
    gx_font_data_12_close();
}
int gx_font_data_open(void) {
    if (gx_font_data_12_open() || gx_font_data_14_open() || gx_font_data_16_open() ||
        gx_font_data_24_open() || gx_font_data_28_open() || gx_font_data_32_open()) {
        gx_font_data_close();
        return -1;
    }
    return 0;
}
#else
int gx_font_data_open(void) { return 0; }
void gx_font_data_close(void) {}
#endif
