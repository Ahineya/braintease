/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Opt-in drawing measurements. Each fence acknowledges completed server work;
 * it is not a measurement of when the host monitor displays those pixels. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "fde_nano.h"
#include "theme.h"
#ifndef PROFILE_WIDTH
#define PROFILE_WIDTH 320
#define PROFILE_HEIGHT 220
#endif
#define W PROFILE_WIDTH
#define H PROFILE_HEIGHT
#ifndef PROFILE_SAMPLES
#define PROFILE_SAMPLES 12
#endif

static GR_WINDOW_ID window;
static GR_GC_ID gc;
static GR_WINDOW_ID cached;
static FDE_IMAGE theme;

static unsigned long milliseconds(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) exit(2);
    return (unsigned long)t.tv_sec * 1000UL + t.tv_nsec / 1000000UL;
}

static void fence(void)
{
    GR_WINDOW_INFO info;
    GrFlush();
    GrGetWindowInfo(window, &info);
    /* Replies flush before nxr_present(). A second request waits for that
     * preceding presentation as well, without introducing any new damage. */
    GrGetWindowInfo(window, &info);
    if (!info.wid) exit(3);
}

static void frame(GR_DRAW_ID target)
{
    const struct fde_asset *a = &fde_theme_assets[FDE_ASSET_WINDOW_ACTIVE];
    struct fde_skin skin = a->skin;
    skin.centre = 0;
    if (fde_skin_paint(theme, target, gc, a->x, a->y, &skin, 0, 0, W, H)) exit(4);
}

static void operation(unsigned which, unsigned iteration)
{
    unsigned y;
    switch (which) {
    case 0: GrFillRect(window, gc, 0, 0, W, H); break;
    case 1:
        for (y = 0; y < H; ++y) GrFillRect(window, gc, 0, y, W, 1);
        break;
    case 2: frame(window); break;
    case 3: GrCopyArea(window, gc, 0, 0, W, H, cached, 0, 0, MWROP_COPY); break;
    case 4: GrCopyArea(window, gc, 1, 1, W - 2, H - 2, window, 0, 0, MWROP_COPY); break;
    case 5: GrMoveWindow(window, iteration & 1 ? 16 : 32, iteration & 1 ? 16 : 32); break;
    case 6: GrFillRect(cached, gc, 0, 0, W, H); break;
    case 7: frame(cached); break;
    case 8: GrFillRect(window, gc, 0, 0, 1, 1); break;
    case 9: break;
    }
    fence();
}

int main(void)
{
    static const char *names[] = {"fill_rectangle", "fill_rows", "skin_frame",
                                 "cached_frame_copy", "overlap_copy", "move_window",
                                 "offscreen_fill", "offscreen_skin", "single_pixel", "fence_only"};
    unsigned which, iteration;
    unsigned long start;
    if (GrOpen() < 0) return 1;
    window = GrNewWindow(GR_ROOT_WINDOW_ID, 16, 16, W, H, 0, 0x30383e, 0);
    gc = GrNewGC();
    if (!window || !gc) return 2;
    GrMapWindow(window); GrSetGCForeground(gc, 0x65757e); fence();
    start = milliseconds();
    theme = fde_image_load_file("/usr/share/fusede/themes/slate/theme.fdi");
    if (!theme) return 4;
    fence();
    printf("FDE_PROFILE theme_load 0 %lu\n", milliseconds() - start); fflush(stdout);
    cached = GrNewPixmapEx(W, H, MWIF_RGB565, NULL);
    if (!cached) return 5;
    GrFillRect(cached, gc, 0, 0, W, H); frame(cached); fence();
    for (which = 0; which < sizeof(names) / sizeof(names[0]); ++which) {
        for (iteration = 0; iteration < PROFILE_SAMPLES; ++iteration) {
            start = milliseconds();
            operation(which, iteration);
            printf("FDE_PROFILE %s %u %lu\n", names[which], iteration, milliseconds() - start);
            fflush(stdout);
        }
    }
    GrClose();
    puts("FDE_PROFILE_DONE");
    return 0;
}
