#include <curses.h>
#include "curspriv.h"

/****************************************************************/
/* Winsertln() inserts a blank line instead of the cursor line	*/
/* In window 'win' and pushes other lines down.			*/
/****************************************************************/

int winsertln(WINDOW *win)
{
  chtype *temp, *end, blank;
  int y;

  win->_last_valid = 0;
  blank = ' ' | (win->_attrs & ATR_MSK);
  temp = win->_line[win->_regbottom];
  for (y = win->_regbottom; y > win->_cury; y--) {
	win->_line[y] = win->_line[y - 1];
	win->_minchng[y] = 0;
	win->_maxchng[y] = win->_maxx;
  }
  win->_line[win->_cury] = temp;
  for (end = &temp[win->_maxx]; temp <= end; temp++) *temp = blank;
  win->_minchng[win->_cury] = 0;
  win->_maxchng[win->_cury] = win->_maxx;
  return(OK);
}
