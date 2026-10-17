/* SPDX-License-Identifier: MIT */
#include "tui_curses.h"
int tui_curses_install(const TuiTheme *theme, unsigned first, chtype *styles, const chtype *fallback) {
    unsigned i, n = tui_theme_count(theme), flags;
    int colored;
    if (!styles || first < 1 || first > 63 || n > 64 - first) return ERR;
    colored = start_color() == OK;
    ripple_curses_lines(tui_theme_lines(theme));
    for (i = 0; i < n; ++i) {
        flags = tui_theme_flags(theme, i);
        styles[i] = (flags & TUI_BOLD ? A_BOLD : 0) |
                    (flags & TUI_REVERSE ? A_REVERSE : 0) |
                    (flags & TUI_UNDERLINE ? A_UNDERLINE : 0);
        if (colored && init_pair(first + i, tui_theme_fg(theme, i), tui_theme_bg(theme, i)) == OK)
            styles[i] |= COLOR_PAIR(first + i);
        else if (fallback) styles[i] |= fallback[i];
    }
    return OK;
}
