/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <curses.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "curspriv.h"

static int ended = 1, cleanup_registered;
static void replace_window(WINDOW *destination, WINDOW *replacement) {
    WINDOW old = *destination;
    *destination = *replacement;
    *replacement = old;
    delwin(replacement);
}
int isendwin(void) { return ended; }
int def_shell_mode(void) { return tcgetattr(0, &_orig_tty) < 0 ? ERR : OK; }
int def_prog_mode(void) { return tcgetattr(0, &_tty) < 0 ? ERR : OK; }
int reset_shell_mode(void) { return tcsetattr(0, TCSANOW, &_orig_tty) < 0 ? ERR : OK; }
int reset_prog_mode(void) {
    if (!stdscr || tcsetattr(0, TCSANOW, &_tty) < 0) return ERR;
    if (ended) { fputs("\033[?1049h", stdout); fflush(stdout); }
    ended = 0; clearok(stdscr, TRUE); ripple_curses_invalidate(); return OK;
}
int endwin(void) {
    if (ended) return OK;
    fflush(stdout);
    /* Exit paths must restore attributes/cursor and input, even when screen
     * allocations or rendering have failed. Windows survive suspend/resume. */
    fputs("\033[0m\033(B\033[?25h\033[0 q\033[?1049l", stdout);
    fflush(stdout);
    if (reset_shell_mode() == ERR) return ERR;
    ended = 1; return OK;
}
static void cleanup(void) { endwin(); }
WINDOW *initscr(void) {
    char *term;
    if (stdscr) return reset_prog_mode() == OK ? stdscr : NULL;
    if (def_shell_mode() == ERR) return NULL;
    term = getenv("TERM");
    if (setterm(term ? term : "ansi") == ERR && setterm("ansi") == ERR) return NULL;
    gettmode();
    stdscr = newwin(LINES, COLS, 0, 0);
    curscr = newwin(LINES, COLS, 0, 0);
    _cursvar.tmpwin = newwin(LINES, COLS, 0, 0);
    if (!stdscr || !curscr || !_cursvar.tmpwin) {
        delwin(stdscr); delwin(curscr); delwin(_cursvar.tmpwin);
        stdscr = curscr = _cursvar.tmpwin = NULL;
        return NULL;
    }
    fputs("\033[?1049h", stdout); fflush(stdout);
    ended = 0; clearok(stdscr, TRUE);
    if (!cleanup_registered) { atexit(cleanup); cleanup_registered = 1; }
    return stdscr;
}
int resizeterm(int rows, int cols) {
    struct winsize size;
    WINDOW *screen, *physical, *pending;
    int oldrows = LINES, oldcols = COLS, y, x, i, j;
    if (!stdscr) return ERR;
    if (!rows || !cols) {
        if (ioctl(0, TIOCGWINSZ, &size) < 0) return ERR;
        if (!rows) rows = size.ws_row;
        if (!cols) cols = size.ws_col;
    }
    if (rows < 1 || rows > 256 || cols < 1 || cols > 512) { errno = EINVAL; return ERR; }
    if (rows == LINES && cols == COLS) return OK;
    /* A child aliases its parent's rows: reject rather than free live views. */
    if (stdscr->_children || curscr->_children || _cursvar.tmpwin->_children) {
        errno = EBUSY; return ERR;
    }
    LINES = rows; COLS = cols;
    screen = newwin(rows, cols, 0, 0); physical = newwin(rows, cols, 0, 0);
    pending = newwin(rows, cols, 0, 0);
    if (!screen || !physical || !pending) {
        delwin(screen); delwin(physical); delwin(pending);
        LINES = oldrows; COLS = oldcols; return ERR;
    }
    for (i = 0; i < rows && i < oldrows; ++i)
        for (j = 0; j < cols && j < oldcols; ++j) screen->_line[i][j] = stdscr->_line[i][j];
    y = stdscr->_cury < rows ? stdscr->_cury : rows - 1;
    x = stdscr->_curx < cols ? stdscr->_curx : cols - 1;
    screen->_keypad = stdscr->_keypad; screen->_delay = stdscr->_delay;
    screen->_nodelay = stdscr->_nodelay; screen->_scroll = stdscr->_scroll;
    screen->_attrs = stdscr->_attrs;
    /* WINDOW identities remain stable for applications caching stdscr. */
    replace_window(stdscr, screen); replace_window(curscr, physical);
    replace_window(_cursvar.tmpwin, pending);
    wmove(stdscr, y, x); clearok(stdscr, TRUE);
    return OK;
}
int resize_term(int rows, int cols) { return resizeterm(rows, cols); }
int ripple_curses_resize(void) {
    struct winsize size;
    if (ioctl(0, TIOCGWINSZ, &size) < 0 || !size.ws_row || !size.ws_col ||
        (size.ws_row == LINES && size.ws_col == COLS)) return 0;
    return resizeterm(size.ws_row, size.ws_col) == OK;
}
