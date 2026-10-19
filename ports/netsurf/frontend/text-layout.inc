/* SPDX-License-Identifier: GPL-2.0-only */
#include "../../../runtime/include/ripple_unicode.h"
/* Painting, wrapping, hit testing and selection share the same cell metrics.
 * The Unicode helper retains a direct path for ordinary ASCII strings. */
static nserror text_width(const struct plot_font_style *style, const char *s, size_t n, int *width)
{
    size_t at = 0, columns = 0;
    unsigned cells;
    while (at < n) {
        at = ru_next_grapheme((const unsigned char *)s, n, at, &cells);
        columns += cells;
        if (columns > 4095) return NSERROR_BAD_SIZE;
    }
    *width = (int)(columns * 8); return NSERROR_OK;
}
static nserror text_position(const struct plot_font_style *style, const char *s, size_t n,
                             int x, size_t *offset, int *actual)
{
    size_t at = 0, next;
    unsigned cells;
    int width = 0;
    while (at < n) {
        next = ru_next_grapheme((const unsigned char *)s, n, at, &cells);
        if (width + (int)cells * 4 > x) break;
        if (width > 32760 - (int)cells * 8) return NSERROR_BAD_SIZE;
        at = next; width += cells * 8;
    }
    *offset = at; *actual = width; return NSERROR_OK;
}
static nserror text_split(const struct plot_font_style *style, const char *s, size_t n,
                          int x, size_t *offset, int *actual)
{
    size_t at = 0, last = 0;
    unsigned cells;
    int width = 0, last_width = 0;
    while (at < n) {
        if (s[at] == ' ') {
            if (width > x) { if (!last) { last = at; last_width = width; } break; }
            last = at; last_width = width;
        }
        at = ru_next_grapheme((const unsigned char *)s, n, at, &cells);
        if (width > 32760 - (int)cells * 8) return NSERROR_BAD_SIZE;
        width += cells * 8;
    }
    if ((at == n && width <= x) || !last) { last = n; last_width = width; }
    *offset = last; *actual = last_width; return NSERROR_OK;
}
