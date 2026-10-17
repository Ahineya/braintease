/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ANSI output independent of optional/old termcap attribute strings. */
#include <curses.h>
#include <string.h>
#include "curspriv.h"

static chtype last = ~0UL;
void ripple_curses_invalidate(void) {
    last = ~0UL;
    if (curscr) curscr->_clear = TRUE;
}
static void color(int value, int background) {
    if (value < 0) printf("\033[%dm", background ? 49 : 39);
    else if (value < 8) printf("\033[%dm", (background ? 40 : 30) + value);
    else if (value < 16) printf("\033[%dm", (background ? 100 : 90) + value - 8);
    else printf("\033[%d;5;%dm", background ? 48 : 38, value);
}
static void attributes(chtype cell) {
    chtype attrs = cell & A_ATTRIBUTES;
    int fg, bg;
    if (attrs == last) return;
    last = attrs;
    fputs("\033[0m\033(B", stdout);
    if (attrs & A_BOLD) fputs("\033[1m", stdout);
    if (attrs & A_BLINK) fputs("\033[5m", stdout);
    if (attrs & (A_REVERSE | A_STANDOUT)) fputs("\033[7m", stdout);
    if (attrs & A_UNDERLINE) fputs("\033[4m", stdout);
    if (PAIR_NUMBER(attrs) && pair_content(PAIR_NUMBER(attrs), &fg, &bg) == OK) {
        color(fg, 0); color(bg, 1);
    }
    if (attrs & A_ALTCHARSET) fputs("\033(0", stdout);
}
void wnoutrefresh(WINDOW *win) {
    WINDOW *pending = _cursvar.tmpwin;
    int y, sy, sx, first, count;
    if (!win || !pending) return;
    /* Independent floating windows may outlive a screen resize. Clip safely. */
    for (y = 0; y <= win->_maxy; ++y) {
        sy = y + win->_begy;
        if (sy < 0 || sy >= LINES) continue;
        first = win->_begx < 0 ? -win->_begx : 0;
        sx = win->_begx + first;
        count = win->_maxx + 1 - first;
        if (count > COLS - sx) count = COLS - sx;
        /* Compare complete layers, including unchanged windows: this restores
         * covered cells when an overlay moves or disappears, and sees writes
         * through subwindow aliases. Only changed rows need physical diffing. */
        if (count > 0 && memcmp(pending->_line[sy] + sx, win->_line[y] + first, count * sizeof(chtype))) {
            memcpy(pending->_line[sy] + sx, win->_line[y] + first, count * sizeof(chtype));
            pending->_minchng[sy] = 0; pending->_maxchng[sy] = COLS - 1;
        }
        win->_minchng[y] = win->_maxchng[y] = _NO_CHANGE;
    }
    if (win->_clear) { pending->_clear = TRUE; win->_clear = FALSE; }
    if (!win->_leave) {
        pending->_cury = min(LINES - 1, win->_begy + win->_cury);
        pending->_curx = min(COLS - 1, win->_begx + win->_curx);
    }
}
void doupdate(void) {
    WINDOW *pending = _cursvar.tmpwin;
    int y, x, forced, written = 0;
    if (!pending || !curscr) return;
    if (isendwin() && reset_prog_mode() == ERR) return;
    forced = pending->_clear || curscr->_clear;
    if (forced) {
        last = ~0UL;
        attributes(0);
        fputs("\033[H\033[2J", stdout);
        pending->_clear = curscr->_clear = FALSE;
    }
    for (y = 0; y < LINES; ++y) {
        if (!forced && pending->_minchng[y] == _NO_CHANGE) continue;
        x = 0;
        while (x < COLS) {
            if (!forced && pending->_line[y][x] == curscr->_line[y][x]) { ++x; continue; }
            printf("\033[%d;%dH", y + 1, x + 1);
            written = 1;
            while (x < COLS && (forced || pending->_line[y][x] != curscr->_line[y][x])) {
                chtype cell = pending->_line[y][x];
                attributes(cell); putchar(cell & A_CHARTEXT);
                curscr->_line[y][x++] = cell;
            }
        }
        pending->_minchng[y] = pending->_maxchng[y] = _NO_CHANGE;
    }
    if (forced || written || curscr->_cury != pending->_cury || curscr->_curx != pending->_curx)
        printf("\033[%d;%dH", pending->_cury + 1, pending->_curx + 1);
    curscr->_cury = pending->_cury; curscr->_curx = pending->_curx;
    fflush(stdout);
}
void wrefresh(WINDOW *win) {
    if (win == curscr) { ripple_curses_invalidate(); }
    else wnoutrefresh(win);
    doupdate();
}
