/* SPDX-License-Identifier: MIT */
/* Inspect LVGL's byte-addressed drawing assumptions without changing upstream.
 * This is an investigation program, not a functioning LVGL renderer. */
#include "lvgl.h"
#include <stdio.h>
#include <limits.h>

int main(void)
{
    uint16_t pixels[8] = {0};
    /* lv_draw_buf_goto_xy uses x * bpp / 8 on a uint8_t pointer. */
    uint8_t *second = (uint8_t *)pixels + LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565);
    /* drawbuf_next_row uses the same byte pointer and a byte stride. */
    uint8_t *row = (uint8_t *)pixels + 4 * LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565);
    printf("LVGL_ABI char_bits=%u int_cells=%u pointer_cells=%u u8_cells=%u u16_cells=%u "
           "rgb565_units=%u second_pixel=%u next_row=%u\n",
           (unsigned)CHAR_BIT, (unsigned)sizeof(int), (unsigned)sizeof(void *),
           (unsigned)sizeof(uint8_t), (unsigned)sizeof(uint16_t),
           (unsigned)LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565),
           (unsigned)(second == (uint8_t *)&pixels[1]),
           (unsigned)(row == (uint8_t *)&pixels[4]));
    return 0;
}
