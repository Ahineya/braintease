/* SPDX-License-Identifier: MIT */
#include "font-data.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Font function addresses are retained exactly by the storage transformation.
 * Raster parity is separately exercised by the real LVGL scene tests. */
bool lv_font_get_glyph_dsc_fmt_txt(const lv_font_t *font, lv_font_glyph_dsc_t *out,
                                   uint32_t codepoint, uint32_t next) {
    (void)font; (void)out; (void)codepoint; (void)next;
    return false;
}
const void *lv_font_get_bitmap_fmt_txt(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *draw) {
    (void)glyph; (void)draw;
    return NULL;
}
const lv_font_t gx_symbols_14 = {0}, gx_symbols_28 = {0};
struct face { const lv_font_t *packed, *original; unsigned long octets, glyphs; };
#include "font-data-fixture.h"
static void *allocations[16];
static unsigned calls, fail_at, live;
void *font_data_test_malloc(size_t length) {
    void *memory;
    unsigned i;
    if (++calls == fail_at)
        return NULL;
    memory = malloc(length);
    assert(memory);
    for (i = 0; i < 16 && allocations[i]; i++) {}
    assert(i < 16);
    allocations[i] = memory;
    live++;
    return memory;
}
void font_data_test_free(void *memory) {
    unsigned i;
    if (!memory)
        return;
    for (i = 0; i < 16 && allocations[i] != memory; i++) {}
    assert(i < 16 && live);
    allocations[i] = NULL;
    live--;
    free(memory);
}
static unsigned glyph_id(const lv_font_fmt_txt_dsc_t *d, uint32_t cp) {
    unsigned i;
    for (i = 0; i < d->cmap_num; i++) {
        const lv_font_fmt_txt_cmap_t *map = &d->cmaps[i];
        unsigned offset, n;
        if (cp < map->range_start || cp - map->range_start >= map->range_length)
            continue;
        offset = cp - map->range_start;
        if (map->type == LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY)
            return map->glyph_id_start + offset;
        assert(map->type == LV_FONT_FMT_TXT_CMAP_SPARSE_TINY);
        for (n = 0; n < map->list_length; n++)
            if (map->unicode_list[n] == offset)
                return map->glyph_id_start + n;
    }
    return 0;
}
static void compare(const struct face *f) {
    const lv_font_t *a = f->packed, *b = f->original;
    const lv_font_fmt_txt_dsc_t *x = a->dsc, *y = b->dsc;
    unsigned long i;
    assert(a->line_height == b->line_height && a->base_line == b->base_line);
    assert(a->static_bitmap == 1 && a->static_bitmap == b->static_bitmap);
    assert(a->subpx == b->subpx && a->fallback == b->fallback);
    assert(a->get_glyph_dsc == b->get_glyph_dsc && a->get_glyph_bitmap == b->get_glyph_bitmap);
    assert(a->underline_position == b->underline_position && a->underline_thickness == b->underline_thickness);
    assert(x->glyph_bitmap && !memcmp(x->glyph_bitmap, y->glyph_bitmap, f->octets));
    assert(x->bpp == y->bpp && x->stride == y->stride && x->bitmap_format == y->bitmap_format);
    assert(x->cmap_num == y->cmap_num && x->kern_dsc == y->kern_dsc);
    assert(x->kern_classes == y->kern_classes && x->kern_scale == y->kern_scale);
    for (i = 0; i < f->glyphs; i++) {
        const lv_font_fmt_txt_glyph_dsc_t *p = &x->glyph_dsc[i], *q = &y->glyph_dsc[i];
        assert(p->bitmap_index == q->bitmap_index && p->adv_w == q->adv_w);
        assert(p->box_w == q->box_w && p->box_h == q->box_h);
        assert(p->ofs_x == q->ofs_x && p->ofs_y == q->ofs_y);
        assert(p->bitmap_index <= f->octets && p->box_w * p->box_h <= f->octets - p->bitmap_index);
        assert(!memcmp(x->glyph_bitmap + p->bitmap_index, y->glyph_bitmap + q->bitmap_index,
                       p->box_w * p->box_h));
    }
    for (i = 0; i < x->cmap_num; i++) {
        const lv_font_fmt_txt_cmap_t *p = &x->cmaps[i], *q = &y->cmaps[i];
        uint32_t cp;
        assert(p->range_start == q->range_start && p->range_length == q->range_length);
        assert(p->glyph_id_start == q->glyph_id_start && p->type == q->type && p->list_length == q->list_length);
        for (cp = p->range_start; cp < p->range_start + p->range_length; cp++)
            assert(glyph_id(x, cp) == glyph_id(y, cp));
    }
}
static void empty(void) {
    unsigned i;
    assert(!live);
    for (i = 0; i < sizeof(faces) / sizeof(faces[0]); i++)
        assert(!((const lv_font_fmt_txt_dsc_t *)faces[i].packed->dsc)->glyph_bitmap);
}
int main(void) {
    unsigned i, round;
    const uint8_t *pointers[6];
    lv_font_fmt_txt_dsc_t small = {0};
    const uint16_t odd[] = {0xabcd, 0x12ef};
    gx_font_data_close();
    empty();
    for (i = 1; i <= 6; i++) {
        calls = 0; fail_at = i;
        assert(gx_font_data_open() == -1);
        assert(calls == i);
        empty();
        gx_font_data_close();
        empty();
    }
    fail_at = 0;
    for (round = 0; round < 3; round++) {
        unsigned before;
        assert(!gx_font_data_open() && live == 6);
        before = calls;
        for (i = 0; i < 6; i++) {
            compare(&faces[i]);
            pointers[i] = ((const lv_font_fmt_txt_dsc_t *)faces[i].packed->dsc)->glyph_bitmap;
        }
        assert(!gx_font_data_open() && calls == before && live == 6);
        for (i = 0; i < 6; i++)
            assert(pointers[i] == ((const lv_font_fmt_txt_dsc_t *)faces[i].packed->dsc)->glyph_bitmap);
        gx_font_data_close(); empty();
        gx_font_data_close(); empty();
    }
    assert(!gx_font_data_expand(&small, odd, 3));
    assert(small.glyph_bitmap[0] == 0xcd && small.glyph_bitmap[1] == 0xab && small.glyph_bitmap[2] == 0xef);
    gx_font_data_release(&small);
    gx_font_data_release(&small);
    assert(!live);
    assert(gx_font_data_expand(NULL, odd, 3) == -1);
    assert(gx_font_data_expand(&small, NULL, 3) == -1);
    assert(gx_font_data_expand(&small, odd, 0) == -1);
    assert(gx_font_data_expand(&small, odd, 1048577UL) == -1);
    puts("FONT_DATA passed: all six original A8 faces/metrics/maps, stable pointers, every allocation rollback, repeated lifecycle, odd octet tail");
    return 0;
}
