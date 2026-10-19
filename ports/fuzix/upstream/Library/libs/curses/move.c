#include <curses.h>
#include "curspriv.h"

int wmove(WINDOW *win, int y, int x)
{
	if ((x < 0) || (x > win->_maxx) || (y < win->_regtop) || (y > win->_regbottom))
		return (ERR);

	win->_last_valid = 0;
	win->_grapheme.started = 0;
	win->_curx = x;
	win->_cury = y;
	return (OK);
}
