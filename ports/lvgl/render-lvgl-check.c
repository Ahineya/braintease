/* SPDX-License-Identifier: MIT */
/* Mixed native/CPU strip replay; all pixel ownership remains inside GX. */
#include "ui.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static void image_draw(lv_event_t *event) {
    lv_draw_image_dsc_t d;
    lv_area_t area;
    lv_draw_image_dsc_init(&d);
    d.src = lv_event_get_user_data(event);
    lv_obj_get_coords(lv_event_get_target_obj(event), &area);
    lv_draw_image(lv_event_get_layer(event), &d, &area);
}
static void panel(int x, int y, int width, int height, uint32_t colour, unsigned opacity) {
    lv_obj_t *o = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    gx_lv_pos(o, x, y); gx_lv_size(o, width, height);
    lv_obj_set_style_bg_color(o, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_opa(o, opacity, 0);
}
int main(int argc, char **argv) {
    lv_image_dsc_t image;
    uint8_t *pixels = NULL;
    unsigned density, x, y, width, height;
    lv_obj_t *o;
    int mixed = argc > 1 && !strcmp(argv[1], "mixed"), status;
    assert(!gx_lv_open("Retained renderer scene", 800, 560));
    density = gx_lv_density();
    ui_screen();
    panel(22, 18, 756, 520, 0x273e59UL, LV_OPA_COVER);
    o = ui_label(lv_screen_active(), "Native fills and text before the image", UI_HEADING);
    gx_lv_pos(o, 37, 30);
    panel(45, 66, 620, 435, 0x9d6139UL, 147);
    o = ui_label(lv_screen_active(), "Ukrainian: Україна  |  alpha 0..255", UI_BODY);
    gx_lv_pos(o, 52, 77);
    if (mixed) {
        width = 430 * density; height = 390 * density;
        pixels = malloc((unsigned long)width * height * 3);
        assert(pixels);
        for (y = 0; y < height; y++) for (x = 0; x < width; x++) {
            uint8_t *p = pixels + ((unsigned long)y * width + x) * 3;
            p[0] = (x * 3 + y) & 255;
            p[1] = (x + y * 3) & 255;
            p[2] = (x * 7 + y * 5) & 255;
        }
        memset(&image, 0, sizeof(image));
        image.header.magic = LV_IMAGE_HEADER_MAGIC;
        image.header.cf = LV_COLOR_FORMAT_RGB888;
        image.header.w = width; image.header.h = height;
        image.header.stride = width * 3;
        image.data_size = (unsigned long)width * height * 3;
        image.data = pixels;
        o = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(o);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(o, image_draw, LV_EVENT_DRAW_MAIN, &image);
        gx_lv_pos(o, 150, 105); gx_lv_size(o, 430, 390);
    }
    panel(100, 180, 550, 63, 0x375129UL, 179);
    o = ui_label(lv_screen_active(), "Text after the image must retain earlier pixels", UI_BODY);
    gx_lv_pos(o, 117, 200);
    panel(400, 449, 330, 58, 0xa73869UL, LV_OPA_COVER);
    o = ui_label(lv_screen_active(), "Last strip: 12345 / abcdef", UI_HEADING);
    gx_lv_pos(o, 413, 465);
    lv_refr_now(NULL);
    puts("RENDER_SCENE ready");
    fflush(stdout);
    status = gx_lv_run();
    gx_lv_close(); free(pixels);
    return status;
}
