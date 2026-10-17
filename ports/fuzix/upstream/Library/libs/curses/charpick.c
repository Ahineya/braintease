#include <curses.h>
#include "curspriv.h"

/****************************************************************/
/* Winch(win) returns the character at the current position in	*/
/* Window 'win'.						*/
/****************************************************************/

chtype winch(WINDOW *win)
{
  return((win->_line[win->_cury][win->_curx]));
}				/* winch */

/****************************************************************/
/* Mvinch() moves the stdscr cursor to a new position, then	*/
/* Returns the character at that position.			*/
/****************************************************************/

chtype mvinch(int y, int x)
{
  if (wmove(stdscr, y, x) == ERR) return(ERR);
  return((stdscr->_line[stdscr->_cury][stdscr->_curx]));
}

/****************************************************************/
/* Mvwinch() moves the cursor of window 'win' to a new posi-	*/
/* Tion, then returns the character at that position.		*/
/****************************************************************/

chtype mvwinch(WINDOW *win, int y, int x)
{
  if (wmove(win, y, x) == ERR) return(ERR);
  return((win->_line[win->_cury][win->_curx]));
}
