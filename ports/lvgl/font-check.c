/* SPDX-License-Identifier: MIT */
#include "ui.h"
#include <stdio.h>
#include <string.h>
extern uint32_t (*const lv_text_encoded_next)(const char *, uint32_t *);
extern const lv_font_t gx_font_12, gx_font_14, gx_font_16, gx_font_24, gx_font_28, gx_font_32;
int font_check(void) {
    uint32_t symbol_position = 0;
    if (strlen(LV_SYMBOL_OK) != 3 ||
        lv_text_encoded_next(LV_SYMBOL_OK, &symbol_position) != 0xf00cUL || symbol_position != 3)
        return 1;
    static const uint32_t letters[] = {0x41,  0xe9,  0x490, 0x491, 0x404, 0x454,
                                       0x406, 0x456, 0x407, 0x457, 0x416, 0x3c0, 0xf00cUL};
    const lv_font_t *fonts[6];
    unsigned n, i;
    uint32_t x, y;
    fonts[0] = &gx_font_12;
    fonts[1] = &gx_font_14;
    fonts[2] = &gx_font_16;
    fonts[3] = &gx_font_24;
    fonts[4] = &gx_font_28;
    fonts[5] = &gx_font_32;
    for (n = 0; n < 6; n++)
        for (i = 0; i < sizeof(letters) / sizeof(letters[0]); i++) {
            lv_font_glyph_dsc_t g;
            const uint8_t *map;
            unsigned ink = 0, edge = 0;
            memset(&g, 0, sizeof(g));
            if (!lv_font_get_glyph_dsc(fonts[n], &g, letters[i], 0) || g.is_placeholder ||
                g.format != 8 || !g.box_w || !g.box_h)
                return 1;
            map = lv_font_get_glyph_static_bitmap(&g);
            if (!map)
                return 1;
            for (y = 0; y < g.box_h; y++)
                for (x = 0; x < g.box_w; x++) {
                    unsigned value = map[y * g.stride + x];
                    if (value)
                        ink++;
                    if (value && value < 255)
                        edge++;
                }
            if (!ink || !edge) {
                fprintf(stderr, "FONT_CHECK missing mask %lu\n", (unsigned long)letters[i]);
                return 1;
            }
        }
    puts("FONT_CHECK passed antialiased Latin, Greek, Ukrainian and checkmark glyphs at 1x/2x");
    return 0;
}
