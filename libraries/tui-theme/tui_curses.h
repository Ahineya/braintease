/* SPDX-License-Identifier: MIT */
#ifndef RIPPLE_TUI_CURSES_H
#define RIPPLE_TUI_CURSES_H
#include <curses.h>
#include "tui_theme.h"
/* Installs colors/border mode. Caller supplies count() output slots and an
 * optional count() array of fallback attributes for monochrome terminals.
 * Pair numbers first..first+count-1 must fit in 1..63. */
int tui_curses_install(const TuiTheme *theme, unsigned first, chtype *styles, const chtype *fallback);
#endif
