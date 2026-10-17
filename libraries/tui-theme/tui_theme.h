/* SPDX-License-Identifier: MIT */
#ifndef RIPPLE_TUI_THEME_H
#define RIPPLE_TUI_THEME_H
/* A curses companion: no application roles, paths or environment policy.
 * File loads are transactional. Call define() before loading configuration.
 * Each object owns all strings; returned strings remain borrowed. */
typedef struct TuiTheme TuiTheme;
#define TUI_BOLD 1
#define TUI_REVERSE 2
#define TUI_UNDERLINE 4
TuiTheme *tui_theme_new(const char *name, int lines);
void tui_theme_free(TuiTheme *theme);
int tui_theme_define(TuiTheme *theme, const char *role, int fg, int bg, unsigned flags);
int tui_theme_apply(TuiTheme *theme, const char *section, const char *key, const char *value);
int tui_theme_named(TuiTheme *theme, const char *directory, const char *name);
int tui_theme_file(TuiTheme *theme, const char *directory, const char *path, int optional);
int tui_theme_valid_name(const char *name);
const char *tui_theme_error(const TuiTheme *theme);
const char *tui_theme_name(const TuiTheme *theme);
int tui_theme_lines(const TuiTheme *theme);
unsigned tui_theme_count(const TuiTheme *theme);
int tui_theme_fg(const TuiTheme *theme, unsigned index);
int tui_theme_bg(const TuiTheme *theme, unsigned index);
unsigned tui_theme_flags(const TuiTheme *theme, unsigned index);
#endif
