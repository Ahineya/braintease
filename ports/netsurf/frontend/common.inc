/* SPDX-License-Identifier: GPL-2.0-only */
/* Small bounded frontend services; document parsing and layout stay in NetSurf. */
#include <nsutils/time.h>
#include <parserutils/charset/utf8.h>
#include "c16-resources.h"
struct callback { void (*fn)(void *); void *pw; uint64_t due; };
static struct callback callbacks[128];
static nserror schedule(int delay, void (*fn)(void *), void *pw)
{
    unsigned i, slot = 128;
    uint64_t now;
    if (nsu_getmonotonic_ms(&now) != NSUERROR_OK) return NSERROR_INIT_FAILED;
    for (i = 0; i < 128; i++) {
        if (callbacks[i].fn == fn && callbacks[i].pw == pw) {
            callbacks[i].fn = NULL; slot = i; break;
        }
        if (!callbacks[i].fn && slot == 128) slot = i;
    }
    if (delay < 0) return NSERROR_OK;
    if (slot == 128) return NSERROR_NOSPACE;
    callbacks[slot].fn = fn; callbacks[slot].pw = pw;
    callbacks[slot].due = now + (unsigned)delay;
    return NSERROR_OK;
}
static void poll_callbacks(void)
{
    unsigned i;
    uint64_t now;
    if (nsu_getmonotonic_ms(&now) != NSUERROR_OK) { running = false; return; }
    for (i = 0; i < 128; i++) if (callbacks[i].fn && callbacks[i].due <= now) {
        struct callback item = callbacks[i];
        callbacks[i].fn = NULL;
        item.fn(item.pw);
    }
}
static long next_timeout(void)
{
    unsigned i;
    uint64_t now, next = 250;
    if (nsu_getmonotonic_ms(&now) != NSUERROR_OK) return 1;
    for (i = 0; i < 128; i++) if (callbacks[i].fn) {
        if (callbacks[i].due <= now) return -1; /* Nano-X's poll, not block. */
        if (callbacks[i].due - now < next) next = callbacks[i].due - now;
    }
    return (long)next;
}
static const char *filetype(const char *path)
{
    const char *extension = strrchr(path, '.');
    if(tracing)fprintf(stderr,"NETSURF_GUI_FILE %s\n",path);
    if (extension && !strcmp(extension, ".css")) return "text/css";
    if (extension && (!strcmp(extension, ".txt") || !strcmp(extension, ".c") || !strcmp(extension, ".rfl")))
        return "text/plain";
    if (extension && !strcasecmp(extension, ".png")) return "image/png";
    if (extension && !strcasecmp(extension, ".gif")) return "image/gif";
    if (extension && (!strcasecmp(extension, ".jpg") || !strcasecmp(extension, ".jpeg"))) return "image/jpeg";
    if (extension && !strcasecmp(extension, ".bmp")) return "image/bmp";
    return "text/html";
}
static nserror resource(const char *name, const ns_octet_t **data, size_t *length)
{
    const char *value = NULL;
    if (!strcmp(name, "default.css")) value = ns_resource_default;
    if (!strcmp(name, "quirks.css")) value = ns_resource_quirks;
    if (!strcmp(name, "adblock.css")) value = ns_resource_adblock;
    if (!strcmp(name, "internal.css")) value = ns_resource_internal;
    if (!strcmp(name, "user.css")) value = "";
    if (!value) return NSERROR_NOT_FOUND;
    *data = (const ns_octet_t *)value; *length = strlen(value);
    return NSERROR_OK;
}
static nserror release_resource(const ns_octet_t *data) { return NSERROR_OK; }
#include "text-layout.inc"
static void *bitmap_create(int w, int h, enum gui_bitmap_flags flags) {
    RiImage *im;
    if(w<=0||h<=0)return NULL;
    im=ri_image_new(w,h,NULL);if(im)im->opaque=(flags&BITMAP_OPAQUE)!=0;return im;
}
static void bitmap_destroy(void *p) { ri_nano_forget(p); ri_image_free(p); }
static void bitmap_set_opaque(void *p, bool opaque) { ((RiImage *)p)->opaque=opaque; }
static bool bitmap_get_opaque(void *p) { return ((RiImage *)p)->opaque!=0; }
static unsigned char *bitmap_buffer(void *p) { return ((RiImage *)p)->rgba; }
static size_t bitmap_stride(void *p) { return (size_t)((RiImage *)p)->width*4; }
static int bitmap_width(void *p) { return ((RiImage *)p)->width; }
static int bitmap_height(void *p) { return ((RiImage *)p)->height; }
static void bitmap_modified(void *p) { ri_nano_forget(p); }
static nserror bitmap_render(struct bitmap *p, struct hlcache_handle *c) { return NSERROR_NOT_IMPLEMENTED; }
static struct gui_layout_table layout_table = { .width = text_width, .position = text_position, .split = text_split };
static struct gui_fetch_table fetch_table = {
    .filetype = filetype, .get_resource_data = resource, .release_resource_data = release_resource,
};
static struct gui_bitmap_table bitmap_table = {
    .create = bitmap_create, .destroy = bitmap_destroy, .set_opaque = bitmap_set_opaque,
    .get_opaque = bitmap_get_opaque, .get_buffer = bitmap_buffer, .get_rowstride = bitmap_stride,
    .get_width = bitmap_width, .get_height = bitmap_height, .modified = bitmap_modified, .render = bitmap_render,
};
