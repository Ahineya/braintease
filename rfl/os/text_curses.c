/* Optional text/curses adapter. The portable text engine has no terminal dependency. */
#include <curses.h>
#include "../../libraries/text/text.h"

int rfl_text_paint(TextBuffer *buffer, int y, int x, unsigned long row,
                   unsigned long left, unsigned width, unsigned tabstop,
                   const unsigned long *styles, unsigned long selected,
                   unsigned long from, unsigned long to) {
    unsigned long cells[512];
    if (width > 512 || !styles) return ERR;
    if (text_render(buffer, row, left, width, tabstop, styles, selected, from, to, cells) < 0)
        return ERR;
    return ripple_curses_cells(stdscr, y, x, cells, width);
}
