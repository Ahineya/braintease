/* SPDX-License-Identifier: GPL-2.0-only */
#include "../../../runtime/include/ripple_unicode.h"

/* Byte offsets always refer to complete graphemes. Keep a spare sentinel and
 * publish the new line table only after a successful reflow. */
static bool textplain_push_line(struct textplain_line **lines, size_t *capacity,
                               size_t *count, size_t start, size_t end)
{
    if (*count + 1 >= *capacity) {
        size_t next = *capacity + 128;
        struct textplain_line *grown;
        if (next <= *capacity || next > SIZE_MAX / sizeof(**lines)) return false;
        grown = realloc(*lines, next * sizeof(**lines));
        if (!grown) return false;
        *lines = grown; *capacity = next;
    }
    (*lines)[*count].start = start;
    (*lines)[*count].length = end - start;
    ++*count;
    return true;
}

static void textplain_reformat(struct content *c, int width, int height)
{
    textplain_content *text = (textplain_content *)c;
    const unsigned char *data = (const unsigned char *)text->utf8_data;
    size_t n = text->utf8_data_size, at = 0, start = 0, space = 0;
    size_t columns, col = 0, count = 0, capacity = 0;
    struct textplain_line *lines = NULL;
    int character_width;
    nserror error = guit->layout->width(&textplain_style, "ABCDEFGH", 8, &character_width);
    if (error != NSERROR_OK || character_width <= 0) return;
    columns = width > 2 * MARGIN ? ((long)width - 2 * MARGIN) * 8 / character_width : 1;
    if (!columns) columns = 1;
    while (at < n) {
        size_t next, next_col;
        unsigned cells;
        if (data[at] == '\r' || data[at] == '\n') {
            next = at + 1;
            if (next < n && data[next] != data[at] &&
                (data[next] == '\r' || data[next] == '\n')) ++next;
            if (!textplain_push_line(&lines, &capacity, &count, start, at)) goto no_memory;
            at = start = next; col = space = 0;
            continue;
        }
        next = ru_next_grapheme(data, n, at, &cells);
        next_col = data[at] == '\t' ? (col + TAB_WIDTH) & ~(TAB_WIDTH - 1) : col + cells;
        if (col && next_col > columns) {
            size_t end = space > start ? space : at;
            if (!textplain_push_line(&lines, &capacity, &count, start, end)) goto no_memory;
            at = start = end; col = space = 0;
            continue; /* Reconsider the complete next grapheme, never skip it. */
        }
        if (data[at] == ' ') space = next;
        at = next; col = next_col;
    }
    if (!textplain_push_line(&lines, &capacity, &count, start, n)) goto no_memory;
    lines[count].start = n;
    lines[count].length = 0;
    free(text->physical_line);
    text->physical_line = lines;
    text->physical_line_count = count;
    text->formatted_width = width;
    textplain_tab_width = (long)TAB_WIDTH * character_width / 8;
    c->width = width;
    c->height = count * textplain_line_height() + 2 * MARGIN;
    return;
no_memory:
    free(lines);
    content_broadcast_error(c, NSERROR_NOMEM, NULL);
}
