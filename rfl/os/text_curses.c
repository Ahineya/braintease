/* Optional text/curses adapter. The portable text engine has no terminal dependency. */
#ifndef RFL_HOST_TEXT
#include <curses.h>
#endif
#include "../../libraries/text/text.h"

int rfl_text_paint(TextBuffer *buffer, int y, int x, unsigned long row,
                   unsigned long left, unsigned width, unsigned tabstop,
                   const unsigned long *styles, unsigned long selected,
                   unsigned long from, unsigned long to) {
    unsigned long cells[512];
    if (width > 512 || !styles) return ERR;
    if (text_render(buffer, row, left, width, tabstop, styles, selected, from, to, cells) < 0)
        return ERR;
    if (ripple_curses_cells(stdscr, y, x, cells, width) == ERR) return ERR;
    return text_overflow(buffer);
}
/* 0: only the first typed scalar, use ordinary vi recording. 1: inserted a
 * bounded run; -1: edit failure. Pasted controls remain literal text. */
static unsigned long input_scalars[4096];
static unsigned input_count;
unsigned rfl_text_input_count(void) { return input_count; }
unsigned long rfl_text_input_scalar(unsigned index) { return index < input_count ? input_scalars[index] : 0; }
int rfl_text_input(TextBuffer *buffer, unsigned long cursor, unsigned long first,
                   int literal, unsigned textwidth, unsigned tabstop, unsigned long *result) {
    unsigned char bytes[4096]; unsigned n, following; size_t at = 0;
    input_count = 0;
    n = ru_encode(first, bytes);
    following = ripple_curses_text_run(stdscr, bytes + n, sizeof(bytes) - n, literal);
    if (!literal && !following) return 0;
    if (!literal) while (at < n + following) {
        input_scalars[input_count++] = ru_next(bytes, n + following, &at);
    }
    if (text_replace(buffer, cursor, 0, bytes, n + following) < 0) return -1;
    result[0] = cursor + n + following;
    if (!literal && textwidth) {
        long formatted = text_format(buffer, result[0], textwidth, tabstop);
        if (formatted < 0) return -1;
        result[0] = formatted;
    }
    return 1;
}
