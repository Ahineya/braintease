/* SPDX-License-Identifier: MIT */
#include "lvgl.h"
#include "src/draw/sw/blend/lv_draw_sw_blend_to_rgb888.h"
#include "src/draw/sw/blend/lv_draw_sw_blend_private.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
int fill_check(void) {
    static const unsigned widths[] = {1, 2, 3, 5, 17, 31, 255, 1024};
    unsigned k, pad;
    uint32_t x, y;
    for (k = 0; k < sizeof(widths) / sizeof(widths[0]); k++)
        for (pad = 0; pad < 2; pad++) {
            uint32_t w = widths[k], h = 17, stride = w * 3 + pad * 7, n = stride * h + 4;
            uint8_t *data = malloc(n);
            lv_draw_sw_blend_fill_dsc_t d;
            if (!data)
                return 1;
            memset(data, 0x59, n);
            memset(&d, 0, sizeof(d));
            d.dest_buf = data + 2;
            d.dest_w = w;
            d.dest_h = h;
            d.dest_stride = stride;
            d.opa = 255;
            d.color = lv_color_hex(0x123456);
            lv_draw_sw_blend_color_to_rgb888(&d, 3);
            for (y = 0; y < h; y++)
                for (x = 0; x < stride; x++) {
                    unsigned expected = x >= w * 3   ? 0x59
                                        : x % 3 == 0 ? 0x56
                                        : x % 3 == 1 ? 0x34
                                                     : 0x12;
                    if (data[2 + y * stride + x] != expected) {
                        fprintf(stderr, "FILL_CHECK failed width=%lu padding=%u\n",
                                (unsigned long)w, pad);
                        free(data);
                        return 1;
                    }
                }
            if (data[0] != 0x59 || data[1] != 0x59 || data[n - 1] != 0x59 || data[n - 2] != 0x59) {
                free(data);
                return 1;
            }
            free(data);
        }
    puts("FILL_CHECK passed small/large/odd widths, tight/padded rows and guards");
    return 0;
}
