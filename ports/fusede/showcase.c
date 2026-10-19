/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include "fde_nano.h"
#include "theme.h"
#include "icons.h"
static GR_WINDOW_ID window;
static GR_GC_ID gc;
static FDE_IMAGE theme_image, icon_image;
static int width = 560, height = 400, selected, focus, hover = -1, pressed = -1;
static int tracing;
static const char *message = "Tab: buttons   Up/Down: list   Esc: close";
static void fail(unsigned line) {fprintf(stderr, "fdeshowcase failed line %u (image status %u)\n", line, fde_image_status());exit(1);}
#define CHECK(c) do {if (!(c)) fail(__LINE__);} while (0)
static void text(int x, int y, const char *value, GR_COLOR colour) {
    GrSetGCForeground(gc, colour); GrText(window, gc, x, y, value, -1, GR_TFASCII | GR_TFTOP);
}
static void skin(unsigned asset, int x, int y, unsigned w, unsigned h) {
    const struct fde_asset *a = &fde_theme_assets[asset];
    CHECK(fde_skin_paint(theme_image, window, gc, a->x, a->y, &a->skin, x, y, w, h) == 0);
}
static void icon(unsigned name, unsigned size_index, int x, int y, GR_COLOR colour) {
    const struct fde_icon *i = &fde_icons[size_index * FDE_ICON_COUNT + name];
    struct fde_rect r;
    r.x = x; r.y = y; r.sx = i->x; r.sy = i->y;
    r.sw = r.sh = r.width = r.height = i->size;
    GrSetGCForeground(gc, colour);
    CHECK(fde_image_draw(icon_image, window, gc, &r, 1, 0) == 0);
}
static void status(void) {
    GrSetGCForeground(gc, FDE_COLOUR_SURFACE); GrFillRect(window, gc, 18, height - 44, width - 36, 24);
    text(20, height - 40, message, FDE_COLOUR_MUTED);
}
static void button(int n) {
    static const char *labels[] = {"Normal", "Hover", "Pressed", "Disabled", "Default"};
    unsigned assets[] = {FDE_ASSET_BUTTON_NORMAL, FDE_ASSET_BUTTON_HOVER, FDE_ASSET_BUTTON_PRESSED,
                          FDE_ASSET_BUTTON_DISABLED, FDE_ASSET_BUTTON_DEFAULT};
    int x = 20 + n * 104;
    unsigned asset = assets[n];
    if (n != 3 && pressed == n) asset = FDE_ASSET_BUTTON_PRESSED;
    else if (n != 3 && hover == n) asset = FDE_ASSET_BUTTON_HOVER;
    skin(asset, x, 114, 96, 28);
    text(x + (96 - strlen(labels[n]) * 8) / 2, 120, labels[n], n == 3 ? FDE_COLOUR_MUTED : FDE_COLOUR_TEXT);
    if (focus == n && n != 3) {GrSetGCForeground(gc, FDE_COLOUR_ACCENT);GrRect(window, gc, x + 3, 117, 90, 22);}
}
static void row(int n) {
    static const char *labels[] = {"Projects", "Documents", "hello.rfl", "notes.txt"};
    int y = 188 + n * 26;
    GrSetGCForeground(gc, FDE_COLOUR_FIELD); GrFillRect(window, gc, 24, y, width - 48, 26);
    if (selected == n) skin(FDE_ASSET_SELECTION, 24, y, width - 48, 26);
    icon(n < 2 ? FDE_ICON_FOLDER : FDE_ICON_FILE, 0, 32, y + 5, FDE_COLOUR_TEXT);
    text(58, y + 5, labels[n], FDE_COLOUR_TEXT);
}
static void paint(void) {
    int i;
    GrSetGCForeground(gc, FDE_COLOUR_SURFACE); GrFillRect(window, gc, 0, 0, width, height);
    skin(FDE_ASSET_WINDOW_ACTIVE, 8, 8, width - 16, 40);
    text(20, 14, "Slate / RGB565 skins and Lucide icons", FDE_COLOUR_TEXT);
    for (i = 0; i < 12; ++i) icon(i, 1, 24 + i * 42, 66, FDE_COLOUR_TEXT);
    for (i = 0; i < 5; ++i) button(i);
    skin(FDE_ASSET_FIELD, 20, 164, width - 40, 136);
    text(30, 168, "Files", FDE_COLOUR_MUTED);
    for (i = 0; i < 4; ++i) row(i);
    text(20, 316, "16 px", FDE_COLOUR_MUTED); icon(FDE_ICON_SETTINGS, 0, 76, 316, FDE_COLOUR_TEXT);
    text(120, 316, "24 px", FDE_COLOUR_MUTED); icon(FDE_ICON_SETTINGS, 1, 178, 312, FDE_COLOUR_ACCENT);
    text(224, 316, "32 px", FDE_COLOUR_MUTED); icon(FDE_ICON_SETTINGS, 2, 282, 308, FDE_COLOUR_TEXT);
    status(); GrFlush();
}
#include "imagecheck.inc"
int main(int argc, char **argv) {
    GR_EVENT event; GR_WM_PROPERTIES props; GR_WINDOW_INFO info;
    int i, old, hit, check = argc > 1 && !strcmp(argv[1], "--check");
    tracing = getenv("FDE_TRACE") != NULL;
    if (argc > 1 && !strcmp(argv[1], "--foreign")) return foreign_check(argc, argv);
    if (argc > 1 && !check && strcmp(argv[1], "--built-in")) {puts("usage: fdeshowcase [--built-in|--check]\nTab/Enter/Space: controls; arrows: file list; Escape/Q: close.");return !strcmp(argv[1], "--help") ? 0 : 1;}
    CHECK(GrOpen() >= 0);
    if (check) return image_checks(argv[0]);
    window = GrNewWindow(GR_ROOT_WINDOW_ID, 32, 32, width, height, 0, FDE_COLOUR_SURFACE, 0);
    gc = GrNewGC(); CHECK(window && gc);
    memset(&props, 0, sizeof(props)); props.flags = GR_WM_FLAGS_TITLE; props.title = "FuseDE Showcase";
    GrSetWMProperties(window, &props);
    GrSetGCUseBackground(gc, GR_FALSE);
    GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ |
                          GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_MOUSE_POSITION | GR_EVENT_MASK_MOUSE_EXIT |
                          GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP);
    if (argc > 1) {
        theme_image = fde_image_load(FDE_THEME_WIDTH, FDE_THEME_HEIGHT, fde_theme_pixels, fde_theme_mask);
        icon_image = fde_image_load(FDE_ICON_WIDTH, FDE_ICON_HEIGHT, NULL, fde_icon_mask);
    } else {
        theme_image = fde_image_load_file("/usr/share/fusede/themes/slate/theme.fdi");
        icon_image = fde_image_load_file("/usr/share/fusede/icons/lucide/icons.fdi");
    }
    CHECK(theme_image && icon_image);
    GrMapWindow(window); GrSetFocus(window); paint();
    printf("FDE_SHOWCASE_READY %lu\n", (unsigned long)window); fflush(stdout);
    for (;;) {
        GrGetNextEvent(&event);
        if (event.type == GR_EVENT_TYPE_CLOSE_REQ) break;
        if (event.type == GR_EVENT_TYPE_EXPOSURE) paint();
        else if (event.type == GR_EVENT_TYPE_UPDATE && event.update.utype == GR_UPDATE_SIZE) {
            GrGetWindowInfo(window, &info);
            if (info.width < 544 || info.height < 380) {GrResizeWindow(window, 560, 400);continue;}
            width = info.width; height = info.height; paint();
        } else if (event.type == GR_EVENT_TYPE_KEY_DOWN) {
            if (event.keystroke.ch == MWKEY_ESCAPE || event.keystroke.ch == 'q') break;
            if (event.keystroke.ch == MWKEY_TAB) {
                old = focus; do {focus = (focus + 1) % 5;} while (focus == 3);
                button(old); button(focus);
            } else if (event.keystroke.ch == MWKEY_UP || event.keystroke.ch == MWKEY_DOWN) {
                old = selected; selected = (selected + (event.keystroke.ch == MWKEY_UP ? 3 : 1)) % 4;
                row(old); row(selected);
            } else if (event.keystroke.ch == MWKEY_ENTER || event.keystroke.ch == ' ') {
                message = "Button activated"; status();
            }
            GrFlush();
            if (tracing) {printf("FDE_INPUT %d %d\n", selected, focus);fflush(stdout);}
        } else if (event.type == GR_EVENT_TYPE_MOUSE_POSITION || event.type == GR_EVENT_TYPE_MOUSE_EXIT) {
            hit = event.type == GR_EVENT_TYPE_MOUSE_EXIT ? -1 :
                (event.mouse.y >= 114 && event.mouse.y < 142 && event.mouse.x >= 20 && event.mouse.x < 532 ?
                 (event.mouse.x - 20) / 104 : -1);
            if (hit >= 0 && event.mouse.x >= 20 + hit * 104 + 96) hit = -1;
            if (hit != hover) {old = hover; hover = hit; if (old >= 0) button(old); if (hover >= 0) button(hover); GrFlush();}
        } else if (event.type == GR_EVENT_TYPE_BUTTON_DOWN || event.type == GR_EVENT_TYPE_BUTTON_UP) {
            if (event.type == GR_EVENT_TYPE_BUTTON_DOWN) {
                if (hover >= 0 && hover != 3) {pressed = focus = hover;for (i = 0; i < 5; ++i) button(i);}
                if (event.button.y >= 188 && event.button.y < 292 && event.button.x >= 24 && event.button.x < width - 24) {
                    old = selected; selected = (event.button.y - 188) / 26;row(old);row(selected);
                }
            } else if (pressed >= 0) {
                old = pressed; pressed = -1;button(old);
                if (hover == old) {message = "Button activated";status();}
            }
            GrFlush();
        }
    }
    CHECK(fde_image_destroy(icon_image) == 0 && fde_image_destroy(theme_image) == 0);
    GrDestroyGC(gc); GrDestroyWindow(window); GrClose(); puts("FDE_SHOWCASE_CLOSED"); return 0;
}
