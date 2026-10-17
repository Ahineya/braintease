/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <curses.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

int COLORS = 256, COLOR_PAIRS = 64;
static int foreground[64], background[64], started;
int has_colors(void) {
    char *term = getenv("TERM");
    return !getenv("NO_COLOR") && (!term || strcmp(term, "dumb"));
}
int start_color(void) {
    int i;
    if (!stdscr || !has_colors()) return ERR;
    if (!started) {
        for (i = 0; i < 64; ++i) foreground[i] = background[i] = -1;
        started = 1;
    }
    return OK;
}
int use_default_colors(void) { return started ? OK : ERR; }
int init_pair(int pair, int fg, int bg) {
    if (!started || pair < 1 || pair >= COLOR_PAIRS || fg < -1 || fg >= COLORS || bg < -1 || bg >= COLORS) {
        errno = EINVAL; return ERR;
    }
    foreground[pair] = fg; background[pair] = bg;
    /* Existing cells refer to pairs by number; repaint after redefinition. */
    ripple_curses_invalidate();
    return OK;
}
int pair_content(int pair, int *fg, int *bg) {
    if (!started || pair < 0 || pair >= COLOR_PAIRS || !fg || !bg) { errno = EINVAL; return ERR; }
    *fg = foreground[pair]; *bg = background[pair]; return OK;
}
void ripple_curses_lines(int enabled) {
    ACS_ULCORNER = enabled ? A_ALTCHARSET | 'l' : '+';
    ACS_URCORNER = enabled ? A_ALTCHARSET | 'k' : '+';
    ACS_LLCORNER = enabled ? A_ALTCHARSET | 'm' : '+';
    ACS_LRCORNER = enabled ? A_ALTCHARSET | 'j' : '+';
    ACS_HLINE = enabled ? A_ALTCHARSET | 'q' : '-';
    ACS_VLINE = enabled ? A_ALTCHARSET | 'x' : '|';
    ACS_PLUS = enabled ? A_ALTCHARSET | 'n' : '+';
    ACS_LTEE = enabled ? A_ALTCHARSET | 't' : '+';
    ACS_RTEE = enabled ? A_ALTCHARSET | 'u' : '+';
    ACS_TTEE = enabled ? A_ALTCHARSET | 'w' : '+';
    ACS_BTEE = enabled ? A_ALTCHARSET | 'v' : '+';
}
