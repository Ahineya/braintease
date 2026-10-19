/* The portable text engine remains compiled with RCC's compatibility ABI. */
#include <ripple_unicode.h>
void *rfl_curses_screen(void);
int rfl_curses_cells(void *, int, int, const unsigned long *, int);
unsigned rfl_host_text_run(void *, unsigned char *, unsigned, int);
#define ERR (-1)
#define stdscr rfl_curses_screen()
#define ripple_curses_cells rfl_curses_cells
#define ripple_curses_text_run rfl_host_text_run
#define RFL_HOST_TEXT 1
#include "../os/text_curses.c"
