/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <curses.h>
#include <errno.h>
#include <stdlib.h>
#include "curspriv.h"

static WINDOW *create(int rows, int cols, int y, int x) {
    WINDOW *w;
    int i;
    if (y < 0 || x < 0 || rows < 1 || cols < 1 || rows > 256 || cols > 512 ||
        (long)y + rows > LINES || (long)x + cols > COLS) {
        errno = EINVAL; return NULL;
    }
    w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->_maxy = rows - 1; w->_maxx = cols - 1;
    w->_begy = y; w->_begx = x; w->_tabsize = 8; w->_delay = -1;
    w->_regbottom = rows - 1;
    w->_line = calloc(rows, sizeof(*w->_line));
    w->_minchng = calloc(rows, sizeof(*w->_minchng));
    w->_maxchng = calloc(rows, sizeof(*w->_maxchng));
    if (!w->_line || !w->_minchng || !w->_maxchng) { delwin(w); return NULL; }
    for (i = 0; i < rows; ++i) w->_maxchng[i] = cols - 1;
    if (y + rows == LINES) w->_flags |= _ENDLINE;
    if (x == 0 && y == 0 && rows == LINES && cols == COLS) w->_flags |= _FULLWIN;
    if (x + cols == COLS && y + rows == LINES) w->_flags |= _SCROLLWIN;
    return w;
}
WINDOW *newwin(int rows, int cols, int y, int x) {
    WINDOW *w;
    int i, j;
    if (!rows) rows = LINES - y;
    if (!cols) cols = COLS - x;
    w = create(rows, cols, y, x);
    if (!w) return NULL;
    for (i = 0; i < rows; ++i) {
        w->_line[i] = calloc(cols, sizeof(chtype));
        if (!w->_line[i]) { delwin(w); return NULL; }
        for (j = 0; j < cols; ++j) w->_line[i][j] = ' ';
    }
    return w;
}
WINDOW *subwin(WINDOW *parent, int rows, int cols, int y, int x) {
    WINDOW *w;
    int i;
    if (!parent || y < parent->_begy || x < parent->_begx) return NULL;
    if (!rows) rows = parent->_maxy + 1 - (y - parent->_begy);
    if (!cols) cols = parent->_maxx + 1 - (x - parent->_begx);
    if ((long)y + rows > (long)parent->_begy + parent->_maxy + 1 ||
        (long)x + cols > (long)parent->_begx + parent->_maxx + 1 || parent->_children == 32767)
        return NULL;
    w = create(rows, cols, y, x);
    if (!w) return NULL;
    w->_flags |= _SUBWIN; w->_parent = parent; ++parent->_children;
    for (i = 0; i < rows; ++i) w->_line[i] = parent->_line[y - parent->_begy + i] + x - parent->_begx;
    return w;
}
int delwin(WINDOW *w) {
    int i;
    if (!w) return ERR;
    if (w->_children) { errno = EBUSY; return ERR; }
    if (w->_parent) --w->_parent->_children;
    if (!(w->_flags & _SUBWIN) && w->_line)
        for (i = 0; i <= w->_maxy; ++i) free(w->_line[i]);
    free(w->_line); free(w->_minchng); free(w->_maxchng); free(w);
    return OK;
}
int wbkgd(WINDOW *w, chtype style) {
    int y, x;
    if (!w) return ERR;
    w->_attrs = style & A_ATTRIBUTES;
    for (y = 0; y <= w->_maxy; ++y)
        for (x = 0; x <= w->_maxx; ++x) w->_line[y][x] = ' ' | w->_attrs;
    touchwin(w); return OK;
}
int wscrl(WINDOW *w, int lines) {
    int y, x, source;
    long distance = lines;
    if (!w || !w->_scroll) return ERR;
    if (distance > w->_regbottom - w->_regtop + 1) distance = w->_regbottom - w->_regtop + 1;
    if (distance < -(w->_regbottom - w->_regtop + 1)) distance = -(w->_regbottom - w->_regtop + 1);
    /* Copy cells, not row pointers: derived windows must retain parent aliases. */
    for (y = distance >= 0 ? w->_regtop : w->_regbottom;
         y >= w->_regtop && y <= w->_regbottom; y += distance >= 0 ? 1 : -1) {
        source = y + (int)distance;
        for (x = 0; x <= w->_maxx; ++x)
            w->_line[y][x] = source >= w->_regtop && source <= w->_regbottom
                ? w->_line[source][x] : ' ' | (w->_attrs & A_ATTRIBUTES);
    }
    touchwin(w); return OK;
}
void scroll(WINDOW *w) { if (w && wscrl(w, 1) == OK && w->_cury > w->_regtop) --w->_cury; }
