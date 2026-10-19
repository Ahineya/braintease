/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <curses.h>
#include <errno.h>
/* Explicit per-cell attributes, independent of the window's current style.
 * Clipped writes do not wrap or scroll at the lower-right corner. */
int ripple_curses_cells(WINDOW *w, int row, int column, const chtype *cells, int count) {
    int i, n;
    if (!w || row < 0 || row > w->_maxy || column < 0 || column > w->_maxx || count < 0 || (count && !cells)) {
        errno = EINVAL; return ERR;
    }
    n = w->_maxx - column + 1; if (n > count) n = count;
    for (i = 0; i < n; ++i) {
        chtype cell = cells[i] & (A_ATTRIBUTES | A_CHARTEXT);
        uint32_t ch = cell & A_CHARTEXT;
        if (ch < 32 || (ch >= 127 && ch < 160)) cell = (cell & A_ATTRIBUTES) | '?';
        else if (ch != RU_CONTINUATION && ((ch >= 0xd800 && ch <= 0xdfff) || ch >= RU_CLUSTER_BASE + RU_CLUSTER_LIMIT))
            cell = (cell & A_ATTRIBUTES) | RU_REPLACEMENT;
        w->_line[row][column + i] = cell;
    }
    w->_last_valid = 0;
    if (n) {
        if (w->_minchng[row] < 0 || w->_minchng[row] > column) w->_minchng[row] = column;
        if (w->_maxchng[row] < column + n - 1) w->_maxchng[row] = column + n - 1;
        w->_cury = row; w->_curx = column + n > w->_maxx ? w->_maxx : column + n;
    }
    return OK;
}
int ripple_curses_cursor_shape(int shape) {
    if (shape < 0 || shape > 3) { errno = EINVAL; return ERR; }
    fputs(shape == 1 ? "\033[2 q" : shape == 2 ? "\033[4 q" : shape == 3 ? "\033[6 q" : "\033[0 q", stdout);
    fflush(stdout); return OK;
}
