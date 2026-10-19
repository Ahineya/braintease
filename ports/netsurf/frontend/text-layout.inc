/* SPDX-License-Identifier: GPL-2.0-only */
#include "../../../runtime/include/ripple_unicode.h"
/* Headless acceptance retains cell metrics; GUI supplies proportional glyph
 * advances. Both paths operate on complete graphemes and UTF-8 byte offsets. */
#ifndef TEXT_ADVANCE
static unsigned text_cell_advance(const struct plot_font_style *style,const char *s,size_t n,size_t at,size_t *next)
{ unsigned cells;*next=ru_next_grapheme((const unsigned char *)s,n,at,&cells);return cells*8; }
#define TEXT_ADVANCE text_cell_advance
#endif
static nserror text_width(const struct plot_font_style *style, const char *s, size_t n, int *width)
{
    size_t at = 0, next;
    unsigned advance;long total=0;
    while (at < n) {
        advance=TEXT_ADVANCE(style,s,n,at,&next);at=next;
        total+=advance;
        if(total>32760)return NSERROR_BAD_SIZE;
    }
    *width = (int)total; return NSERROR_OK;
}
static nserror text_position(const struct plot_font_style *style, const char *s, size_t n,
                             int x, size_t *offset, int *actual)
{
    size_t at = 0, next;
    unsigned advance;
    int width = 0;
    while (at < n) {
        advance=TEXT_ADVANCE(style,s,n,at,&next);
        if ((long)width + (advance+1)/2 > x) break;
        if (width > 32760 - (int)advance) return NSERROR_BAD_SIZE;
        at = next; width += advance;
    }
    *offset = at; *actual = width; return NSERROR_OK;
}
static nserror text_split(const struct plot_font_style *style, const char *s, size_t n,
                          int x, size_t *offset, int *actual)
{
    size_t at = 0, next, last = 0;
    unsigned advance;
    int width = 0, last_width = 0;
    while (at < n) {
        if (s[at] == ' ') {
            if (width > x) { if (!last) { last = at; last_width = width; } break; }
            last = at; last_width = width;
        }
        advance=TEXT_ADVANCE(style,s,n,at,&next);at=next;
        if (width > 32760 - (int)advance) return NSERROR_BAD_SIZE;
        width += advance;
    }
    if ((at == n && width <= x) || !last) { last = n; last_width = width; }
    *offset = last; *actual = last_width; return NSERROR_OK;
}
