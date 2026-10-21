/* SPDX-License-Identifier: MIT */
/* Test-only deterministic redraw workload using the actual selector UI. */
#define main selector_application_main
#include "selector.c"
#undef main
#include <time.h>
int fill_check(void);
int font_check(void);
static uint32_t elapsed_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)t.tv_sec * 1000000UL + t.tv_nsec / 1000UL;
}
int main(int argc, char **argv) {
    unsigned i;
    uint32_t at, scroll = 0, render = 0, start;
    if (argc != 2 || browser_load(&browser, argv[1], 0) < 0)
        return 1;
    if (gx_lv_open("LVGL scroll benchmark", width, height) < 0)
        return 1;
    ui();
    if (fill_check()||font_check())
        return 1;
    lv_obj_update_layout(list);
    lv_refr_now(NULL);
    start = elapsed_us();
    for (i = 0; i < 30; i++) {
        at = elapsed_us();
        lv_obj_scroll_by_bounded(list, 0, gx_lv_px(i < 15 ? -40 : 40), LV_ANIM_OFF);
        lv_obj_update_layout(list);
        scroll += elapsed_us() - at;
        at = elapsed_us();
        lv_refr_now(NULL);
        render += elapsed_us() - at;
    }
    printf("LVGL_BENCH frames=30 wall_us=%lu scroll_us=%lu refresh_us=%lu\n",
           (unsigned long)(elapsed_us() - start), (unsigned long)scroll, (unsigned long)render);
    gx_lv_close();
    free(rows);
    browser_free(&browser);
    return 0;
}
