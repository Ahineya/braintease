/* C ABI adapters only. Layout, input editing and widgets live in RFL. */
#include <curses.h>
#include <errno.h>
static unsigned session_owners;
void *rfl_curses_screen(void) { return stdscr; }
int rfl_curses_begin(void) {
    if (session_owners || !isendwin() || !initscr()) return -1;
    session_owners = 1;
    raw(); noecho(); keypad(stdscr, 1); wtimeout(stdscr, 100); curs_set(0);
    ripple_curses_lines(1);
    return 0;
}
void rfl_curses_end(void) { if (session_owners && --session_owners == 0) endwin(); }
int rfl_curses_rows(void) { return LINES; }
int rfl_curses_columns(void) { return COLS; }
void rfl_curses_clear(void) { werase(stdscr); }
int rfl_curses_move(int row, int column) { return wmove(stdscr, row, column); }
void rfl_curses_style(unsigned long style) { wattrset(stdscr, style); }
void rfl_curses_character(int ch) { waddch(stdscr, ch); }
void rfl_curses_refresh(void) { wrefresh(stdscr); }
int rfl_curses_key(void) {
    int key;
    errno = 0;
    key = wgetch(stdscr);
    if (key == ERR && (feof(stdin) || errno == EIO || errno == EBADF)) return -2;
    return key;
}
void rfl_curses_timeout(int milliseconds) { wtimeout(stdscr, milliseconds); }
void rfl_curses_cursor(int visible) { curs_set(visible); }
void rfl_curses_suspend(void) { endwin(); }
int rfl_curses_resume(void) {
    int result = reset_prog_mode();
    ripple_curses_resize(); clearok(stdscr, 1); curs_set(0);
    return result;
}
int rfl_curses_colors(void) { return start_color() == OK; }
int rfl_curses_pair(int pair, int fg, int bg) { return init_pair(pair, fg, bg); }
void rfl_curses_lines(int enabled) { ripple_curses_lines(enabled); }
void rfl_curses_bracketed(int enabled) { ripple_curses_bracketed_paste(enabled); }
static WINDOW *target(void *window) { return window ? window : stdscr; }
void *rfl_curses_window(int rows, int cols, int row, int col) {
    WINDOW *w;
    if (!stdscr || !session_owners || session_owners == 65535U) return NULL;
    w = newwin(rows, cols, row, col);
    if (w) { ++session_owners; keypad(w, 1); wtimeout(w, 100); }
    return w;
}
void rfl_curses_delete(void *window) { if (window && delwin(window) == OK) rfl_curses_end(); }
int rfl_curses_reposition(void *window, int row, int col) { return mvwin(window, row, col); }
int rfl_curses_height(void *window) { return target(window)->_maxy + 1; }
int rfl_curses_width(void *window) { return target(window)->_maxx + 1; }
void rfl_curses_background(void *window, unsigned long style) { wbkgd(target(window), style); }
/* Synchronous draw scratch: no callback, suspension or retained pointer occurs
 * between filling a chunk and copying it into the window. Shared by all windows. */
unsigned char *rfl_curses_scratch(void) { static unsigned char bytes[512]; return bytes; }
static struct ru_decoder drawing;
static WINDOW *drawing_window;
int rfl_curses_text_begin(void *window, int row, int col, unsigned long style) {
    WINDOW *w = target(window);
    drawing_window = NULL; drawing.need = 0;
    if (row < 0 || row > w->_maxy || col < 0 || col > w->_maxx) return 0;
    wattrset(w, style); wmove(w, row, col); drawing_window = w; return 1;
}
int rfl_curses_text_chunk(unsigned char *bytes, int count, int final) {
    int i = 0, decoded; uint32_t cp;
    if (!drawing_window) return 0;
    while (i < count) {
        decoded = ru_decode(&drawing, bytes[i], &cp);
        if (decoded >= 0) ++i;
        if (decoded) {
            if (cp < 32 || (cp >= 127 && cp < 160)) cp = '?';
            if (ripple_curses_scalar(drawing_window, cp) == ERR) { drawing_window = NULL; return 0; }
        }
    }
    if (final) {
        if (drawing.need) ripple_curses_scalar(drawing_window, RU_REPLACEMENT);
        drawing_window = NULL;
    }
    return 1;
}
void rfl_curses_fill(void *window, int row, int col, int width, unsigned long ch, unsigned long style) {
    WINDOW *w = target(window);
    int i, cells = ru_width(ch);
    if (row < 0 || row > w->_maxy || col < 0 || col > w->_maxx || width < 1) return;
    if (width > w->_maxx - col + 1) width = w->_maxx - col + 1;
    if (cells < 1) { ch = RU_REPLACEMENT; cells = 1; }
    wattrset(w, style); wmove(w, row, col);
    for (i = 0; i + cells <= width; i += cells)
        if (ripple_curses_scalar(w, ch) == ERR) break;
    if (i < width) ripple_curses_scalar(w, ' ');
}
/* -1 timeout, -2 terminal loss, 0 text, 1 function key. */
int rfl_curses_event(void *window, uint32_t *value) {
    int result; errno = 0; result = wget_wch(target(window), value);
    return result == ERR && (feof(stdin) || errno == EIO || errno == EBADF) ? -2 : result;
}
void rfl_curses_border(void *window, int row, int col, int rows, int cols, unsigned long style) {
    WINDOW *w = target(window);
    if (row < 0 || col < 0 || rows < 2 || cols < 2 ||
        (long)row + rows > w->_maxy + 1 || (long)col + cols > w->_maxx + 1) return;
    wattrset(w, style); wbox(w, row, col, row + rows - 1, col + cols - 1, 0, 0);
}
void rfl_curses_stage(void *window) { wnoutrefresh(target(window)); }
void rfl_curses_present(void) { doupdate(); }
int rfl_curses_window_key(void *window) {
    int key;
    errno = 0; key = wgetch(target(window));
    return key == ERR && (feof(stdin) || errno == EIO || errno == EBADF) ? -2 : key;
}
void rfl_curses_window_cursor(void *window, int row, int col, int visible) {
    wmove(target(window), row, col); curs_set(visible);
}
int rfl_curses_scroll(void *window, int lines) {
    WINDOW *w = target(window); scrollok(w, 1); return wscrl(w, lines);
}
int rfl_curses_region(void *window, int top, int bottom) {
    WINDOW *w = target(window);
    if (wmove(w, top, 0) == ERR) return ERR;
    return wsetscrreg(w, top, bottom);
}

int rfl_curses_cells(void *window, int row, int col, const unsigned long *cells, int count) {
    return ripple_curses_cells(target(window), row, col, cells, count);
}
int rfl_curses_shape(int shape) { return ripple_curses_cursor_shape(shape); }
