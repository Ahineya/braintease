/* Acceptance frontend for the actual NetSurf browser/layout/redraw core.
 * GPL-2.0, matching the NetSurf core linked by this test. */
#include <netsurf_profile.h>
#include <parserutils/charset/utf8.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <nsutils/time.h>
#include "utils/nsoption.h"
#include "utils/nsurl.h"
#include "utils/utf8.h"
#include "utils/log.h"
#include "netsurf/netsurf.h"
#include "netsurf/window.h"
#include "netsurf/misc.h"
#include "netsurf/fetch.h"
#include "netsurf/layout.h"
#include "netsurf/plotters.h"
#include "netsurf/bitmap.h"
#include "netsurf/browser_window.h"
#include "netsurf/core_window.h"
#include "desktop/page-info.h"
#include "content/fetch.h"
#include "content/content_factory.h"
#include "c16-resources.h"
#define NETSURF_TRACE 1

static int failures, checks;
static void check(bool condition, const char *name)
{
#ifdef NETSURF_TRACE
    fprintf(stderr, "NETSURF_CHECK %s\n", name);
    fflush(stderr);
#endif
    checks++;
    if (!condition) { failures++; printf("FAIL %s\n", name); }
}
struct pending { void (*callback)(void *); void *context; uint64_t due; };
static struct pending pending[128];
static nserror schedule(int delay, void (*callback)(void *), void *context)
{
    unsigned i, slot = 128;
    uint64_t now;
    if (nsu_getmonotonic_ms(&now) != NSUERROR_OK) return NSERROR_INIT_FAILED;
    for (i = 0; i < 128; i++) {
        if (pending[i].callback == callback && pending[i].context == context) {
            pending[i].callback = NULL; slot = i; break;
        }
        if (pending[i].callback == NULL && slot == 128) slot = i;
    }
    if (delay < 0) return NSERROR_OK;
    if (slot == 128) return NSERROR_NOSPACE;
    pending[slot].callback = callback;
    pending[slot].context = context;
    pending[slot].due = now + (unsigned)delay;
    return NSERROR_OK;
}
static void poll_callbacks(void)
{
    unsigned i;
    uint64_t now;
    if (nsu_getmonotonic_ms(&now) != NSUERROR_OK) return;
    for (i = 0; i < 128; i++) {
        if (pending[i].callback && pending[i].due <= now) {
            struct pending item = pending[i];
            pending[i].callback = NULL;
            item.callback(item.context);
        }
    }
}
struct gui_window { struct browser_window *bw; int sx, sy; };
static struct gui_window window;
static int viewport = 480;
static struct gui_window *window_create(struct browser_window *bw, struct gui_window *existing,
                                         gui_window_create_flags flags)
{
    window.bw = bw; return &window;
}
static void window_destroy(struct gui_window *gw) { gw->bw = NULL; }
static nserror invalidate(struct gui_window *gw, const struct rect *area) { return NSERROR_OK; }
static bool get_scroll(struct gui_window *gw, int *x, int *y) { *x = gw->sx; *y = gw->sy; return true; }
static nserror set_scroll(struct gui_window *gw, const struct rect *area)
{ gw->sx = area->x0; gw->sy = area->y0; return NSERROR_OK; }
static nserror dimensions(struct gui_window *gw, int *width, int *height)
{ *width = viewport; *height = 320; return NSERROR_OK; }
static nserror window_event(struct gui_window *gw, enum gui_window_event event) { return NSERROR_OK; }
static void window_status(struct gui_window *gw, const char *text)
{
    fprintf(stderr, "NETSURF_STATUS_TEXT %s\n", text);
    fflush(stderr);
}

static const char *filetype(const char *path)
{
    const char *extension = strrchr(path, '.');
    if (extension && strcmp(extension, ".css") == 0) return "text/css";
    if (extension && strcmp(extension, ".txt") == 0) return "text/plain";
    return "text/html";
}
static nserror resource(const char *name, const ns_octet_t **data, size_t *length)
{
    const char *text = NULL;
    if (strcmp(name, "default.css") == 0) text = ns_resource_default;
    if (strcmp(name, "quirks.css") == 0) text = ns_resource_quirks;
    if (strcmp(name, "adblock.css") == 0) text = ns_resource_adblock;
    if (strcmp(name, "internal.css") == 0) text = ns_resource_internal;
    if (strcmp(name, "user.css") == 0) text = "";
    if (text == NULL) return NSERROR_NOT_FOUND;
    *data = (const ns_octet_t *)text; *length = strlen(text); return NSERROR_OK;
}
static nserror release_resource(const ns_octet_t *data) { return NSERROR_OK; }

/* The acceptance frontend has fixed cell metrics; offsets remain UTF-8 byte
 * offsets, and widths never multiply two 16-bit intermediate values. */
#include "../frontend/text-layout.inc"
static void *bitmap_create(int width, int height, enum gui_bitmap_flags flags) { return NULL; }
static void bitmap_destroy(void *bitmap) {}
static void bitmap_opaque(void *bitmap, bool opaque) {}
static bool bitmap_get_opaque(void *bitmap) { return false; }
static unsigned char *bitmap_buffer(void *bitmap) { return NULL; }
static size_t bitmap_stride(void *bitmap) { return 0; }
static int bitmap_dimension(void *bitmap) { return 0; }
static void bitmap_modified(void *bitmap) {}
static nserror bitmap_render(struct bitmap *bitmap, struct hlcache_handle *content) { return NSERROR_NOT_IMPLEMENTED; }

static unsigned rectangles, text_runs, backgrounds;
static int maximum_y;
static char drawn[2048];
static size_t drawn_length;
static bool capture_plain, plain_valid;
static char plain_drawn[4096];
static size_t plain_length;
static nserror plot_clip(const struct redraw_context *ctx, const struct rect *area) { return NSERROR_OK; }
static nserror plot_rectangle(const struct redraw_context *ctx, const plot_style_t *style, const struct rect *area)
{
    rectangles++;
    if (style->fill_type == PLOT_OP_TYPE_SOLID && style->fill_colour == 0x00332211UL) backgrounds++;
    return NSERROR_OK;
}
static nserror plot_line(const struct redraw_context *ctx, const plot_style_t *style, const struct rect *area) { return NSERROR_OK; }
static nserror plot_text(const struct redraw_context *ctx, const plot_font_style_t *style,
                         int x, int y, const char *text, size_t length)
{
    if (capture_plain) {
        size_t pos = 0, consumed;
        uint32_t scalar;
        if (style->size <= 0) plain_valid = false;
        while (pos < length) {
            if (parserutils_charset_utf8_to_ucs4((const ns_octet_t *)text + pos,
                    length - pos, &scalar, &consumed) != PARSERUTILS_OK) {
                plain_valid = false; break;
            }
            pos += consumed;
        }
        if (plain_length + length >= sizeof(plain_drawn)) plain_valid = false;
        else { memcpy(plain_drawn + plain_length, text, length); plain_length += length; }
    }
    text_runs++;
    if (y > maximum_y) maximum_y = y;
    if (length + drawn_length + 2 < sizeof(drawn)) {
        memcpy(drawn + drawn_length, text, length); drawn_length += length;
        drawn[drawn_length++] = ' '; drawn[drawn_length] = 0;
    }
    return NSERROR_OK;
}
static nserror plot_polygon(const struct redraw_context *ctx, const plot_style_t *style,
                            const int *points, unsigned count) { return NSERROR_OK; }
static nserror plot_disc(const struct redraw_context *ctx, const plot_style_t *style,
                         int x, int y, int radius) { return NSERROR_OK; }
static nserror plot_arc(const struct redraw_context *ctx, const plot_style_t *style,
                        int x, int y, int radius, int first, int last) { return NSERROR_OK; }
static nserror plot_path(const struct redraw_context *ctx, const plot_style_t *style,
                         const float *points, unsigned count, const float transform[6]) { return NSERROR_NOT_IMPLEMENTED; }
static nserror plot_bitmap(const struct redraw_context *ctx, struct bitmap *bitmap,
                           int x, int y, int width, int height, colour background,
                           bitmap_flags_t flags) { return NSERROR_NOT_IMPLEMENTED; }
static const struct plotter_table plotters = {
    .clip = plot_clip, .rectangle = plot_rectangle, .line = plot_line, .text = plot_text,
    .polygon = plot_polygon, .disc = plot_disc, .arc = plot_arc, .path = plot_path,
    .bitmap = plot_bitmap, .option_knockout = false,
};
static struct gui_misc_table misc_table = { .schedule = schedule };
static struct gui_window_table window_table = {
    .create = window_create, .destroy = window_destroy, .invalidate = invalidate,
    .get_scroll = get_scroll, .set_scroll = set_scroll, .get_dimensions = dimensions, .event = window_event,
    .set_status = window_status,
};
static struct gui_fetch_table fetch_table = {
    .filetype = filetype, .get_resource_data = resource, .release_resource_data = release_resource,
};
static struct gui_layout_table layout_table = { .width = text_width, .position = text_position, .split = text_split };
static struct gui_bitmap_table bitmap_table = {
    .create = bitmap_create, .destroy = bitmap_destroy, .set_opaque = bitmap_opaque,
    .get_opaque = bitmap_get_opaque, .get_buffer = bitmap_buffer, .get_rowstride = bitmap_stride,
    .get_width = bitmap_dimension, .get_height = bitmap_dimension, .modified = bitmap_modified, .render = bitmap_render,
};
static struct netsurf_table tables = {
    .misc = &misc_table, .window = &window_table, .fetch = &fetch_table,
    .layout = &layout_table, .bitmap = &bitmap_table,
};
#include "core-utilities.inc"
#include "talloc-check.inc"
static bool load_finished(struct browser_window *bw)
{
    unsigned rounds;
    uint64_t start, now;
    if (nsu_getmonotonic_ms(&start) != NSUERROR_OK) return false;
    for (rounds = 0; rounds < 30000; rounds++) {
        poll_callbacks();
        if (browser_window_has_content(bw) && !browser_window_stop_available(bw)) return true;
        if (nsu_getmonotonic_ms(&now) != NSUERROR_OK) return false;
        if (now - start > 15000) {
            unsigned i, queued = 0;
            for (i = 0; i < 128; i++) if (pending[i].callback) queued++;
            fprintf(stderr, "NETSURF_LOAD_TIMEOUT rounds=%u elapsed_ms=%lu queued=%u\n",
                    rounds, (unsigned long)(now - start), queued);
            fflush(stderr);
            return false;
        }
        usleep(1000);
    }
    return false;
}

static int info_width, info_height;
static nserror info_size(struct core_window *cw, int width, int height)
{ info_width = width; info_height = height; return NSERROR_OK; }
static const struct core_window_callback_table info_callbacks = { .update_size = info_size };
static void page_info_test(struct browser_window *bw)
{
    struct page_info *info = NULL;
    int width, height;
    struct rect clip = { 0, 0, 480, 320 };
    struct redraw_context redraw = { .interactive = false, .background_images = true, .plot = &plotters };
    check(page_info_create(&info_callbacks, NULL, bw, &info) == NSERROR_OK && info != NULL,
          "page info nested initializer and style pointers");
    if (info == NULL) return;
    check(page_info_get_size(info, &width, &height) == NSERROR_OK && width > 0 && height > 0 &&
          width == info_width && height == info_height, "page info dimensions");
    drawn_length = 0; drawn[0] = 0;
    check(page_info_redraw(info, 0, 0, &clip, &redraw) == NSERROR_OK && strstr(drawn, "Local data"),
          "page info local document rendering");
    page_info_destroy(info);
}

static void file_page_test(struct browser_window *bw)
{
    char path[96], address[120], padding[1000];
    int fd, i;
    nsurl *url = NULL;
    struct rect clip = { 0, 0, 180, 320 };
    struct redraw_context redraw = { .interactive = false, .background_images = true, .plot = &plotters };
    const char *head = "<!doctype html><title>Local file</title><!--";
    const char *tail = "--><p>Streamed file tail</p><table><tr><td>Cell A</td><td>Cell B</td></tr></table>";
    bool written;
    snprintf(path, sizeof(path), "/tmp/netsurf-core-%d.html", (int)getpid());
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    check(fd >= 0, "create private HTML fixture");
    if (fd < 0) return;
    written = write(fd, head, strlen(head)) == (ssize_t)strlen(head);
    memset(padding, 'x', sizeof(padding));
    for (i = 0; i < 10; i++)
        if (write(fd, padding, sizeof(padding)) != sizeof(padding)) written = false;
    if (write(fd, tail, strlen(tail)) != (ssize_t)strlen(tail)) written = false;
    close(fd);
    check(written, "write fixture larger than fetch buffer");
    snprintf(address, sizeof(address), "file://%s", path);
    if (nsurl_create(address, &url) == NSERROR_OK) {
        check(browser_window_navigate(bw, url, NULL, BW_NAVIGATE_HISTORY, NULL, NULL, NULL) == NSERROR_OK,
              "local file navigation");
        nsurl_unref(url);
        check(load_finished(bw), "local file fully loaded");
        check(strcmp(browser_window_get_title(bw), "Local file") == 0, "local file DOM title");
        drawn_length = 0; drawn[0] = 0;
        check(browser_window_redraw(bw, 0, 0, &clip, &redraw), "local file redraw");
        check(strstr(drawn, "Streamed file tail") && strstr(drawn, "Cell A") && strstr(drawn, "Cell B"),
              "file chunks and table reach renderer");
    } else check(false, "local file URL");
    check(unlink(path) == 0, "remove private HTML fixture");
}

#include "textplain-check.inc"

int main(int argc, char **argv)
{
    struct browser_window *bw = NULL;
    nsurl *url = NULL;
    int wide_height, narrow_height;
#if CHAR_BIT == 16
    uintptr_t first_break = (uintptr_t)sbrk(0), last_break;
#endif
    struct rect clip = { 0, 0, 480, 320 };
    struct redraw_context redraw = { .interactive = false, .background_images = true, .plot = &plotters };
    const char *page = "data:text/html;charset=utf-8,<!doctype html><title>C16 Layout</title>"
        "<style>body{background:%23112233;color:%23eeeeee}p{margin:8px}</style>"
        "<h1>NetSurf core</h1><p>Alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu.</p>"
        "<p><a href='next.html'>Local link</a> and <b>bold text</b>.</p>";
    write(2, "NETSURF_CORE start\n", 19);
    check(strlen(ns_resource_default) == sizeof(ns_resource_default) - 1, "embedded base stylesheet length");
    check(strlen(ns_resource_quirks) == sizeof(ns_resource_quirks) - 1, "embedded quirks stylesheet length");
    check(strlen(ns_resource_internal) == sizeof(ns_resource_internal) - 1, "embedded internal stylesheet length");
    check_utilities();
    check_talloc();
    check(netsurf_register(&tables) == NSERROR_OK, "frontend registration");
    check(nslog_init(NULL, &argc, argv) == NSERROR_OK, "logging init");
    check(nsoption_init(NULL, NULL, NULL) == NSERROR_OK, "options init");
    check(nsoption_int(font_size) == 128 && nsoption_int(scale) == 100, "integer option defaults");
    check(nsoption_colour(sys_colour_Highlight) == 0x00c00800UL, "32-bit colour option default");
    nsoption_set_bool(foreground_images, false);
    nsoption_set_bool(background_images, false);
    nsoption_set_bool(incremental_reflow, false);
    check(netsurf_init(NULL) == NSERROR_OK, "browser core init");
    {
        lwc_string *mime = NULL;
        lwc_intern_string("text/css", 8, &mime);
        check(mime && content_factory_type_from_mime_type(mime) == CONTENT_CSS, "CSS MIME handler registration");
        if (mime) lwc_string_unref(mime);
        mime = NULL;
        lwc_intern_string("text/html", 9, &mime);
        check(mime && content_factory_type_from_mime_type(mime) == CONTENT_HTML, "HTML MIME handler registration");
        if (mime) lwc_string_unref(mime);
    }
    if (failures) return 1;
    check(nsurl_create(page, &url) == NSERROR_OK, "data page URL");
    check(browser_window_create(BW_CREATE_HISTORY, url, NULL, NULL, &bw) == NSERROR_OK, "browser window creation");
    nsurl_unref(url);
    if (bw == NULL) return 1;
    check(load_finished(bw), "page fully loaded");
    check(strcmp(browser_window_get_title(bw), "C16 Layout") == 0, "DOM title reaches browser");
    check(browser_window_redraw(bw, 0, 0, &clip, &redraw), "HTML redraw");
    check(text_runs > 5 && rectangles > 1, "renderer emits text and geometry");
    check(strstr(drawn, "NetSurf") && strstr(drawn, "Local link"), "DOM text reaches plotter");
    check(backgrounds > 0, "CSS background reaches plotter with full colour");
    wide_height = maximum_y;
    maximum_y = 0; drawn_length = 0; drawn[0] = 0;
    viewport = 180; clip.x1 = viewport;
    browser_window_reformat(bw, false, viewport, 320);
    check(browser_window_redraw(bw, 0, 0, &clip, &redraw), "narrow viewport redraw");
    narrow_height = maximum_y;
    check(narrow_height > wide_height, "CSS layout wraps on resize");
    printf("NETSURF_LAYOUT wide_y=%d narrow_y=%d text_runs=%u rectangles=%u\n", wide_height, narrow_height, text_runs, rectangles);
    file_page_test(bw);
    page_info_test(bw);
    textplain_test(bw);
#if CHAR_BIT == 16
    last_break = (uintptr_t)sbrk(0);
    check(first_break != UINT32_MAX && last_break != UINT32_MAX, "heap measurement");
    printf("NETSURF_CORE_HEAP growth_cells=%lu\n",
        ((last_break >> 16) * 64000UL + (last_break & 65535UL)) -
        ((first_break >> 16) * 64000UL + (first_break & 65535UL)));
#endif
    browser_window_destroy(bw);
    netsurf_exit();
    nsoption_finalise(NULL, NULL);
    nslog_finalise();
    printf("NETSURF_CORE checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
