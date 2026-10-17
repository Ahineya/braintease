#include <curses.h>
#include "curspriv.h"

static void ttysetflags(void)
{
  _tty.c_iflag |= ICRNL | IXON;
  _tty.c_oflag |= OPOST | ONLCR;
  _tty.c_lflag |= ECHO | ICANON | IEXTEN | ISIG;
  _tty.c_cc[VEOF] = 4;

  if (_cursvar.rawmode) {
	_tty.c_iflag &= ~(ICRNL | IXON);
	_tty.c_oflag &= ~(OPOST);
	_tty.c_lflag &= ~(ICANON | IEXTEN | ISIG);
	_tty.c_cc[VMIN] = 1;
  }
  if (_cursvar.cbrkmode) {
	_tty.c_lflag &= ~(ICANON);
	_tty.c_cc[VMIN] = 1;
        _tty.c_cc[VTIME] = _cursvar.halfdelay;
  }
  /* Curses echoes through waddch; kernel echo would duplicate input. */
  _tty.c_lflag &= ~(ECHO | ECHONL);
  if (NONL) {
	_tty.c_iflag &= ~(ICRNL);
	_tty.c_oflag &= ~(ONLCR);
  }
  tcsetattr(0, TCSANOW, &_tty);
}				/* ttysetflags */

void raw(void)
{
  _cursvar.halfdelay = 0;
  _cursvar.rawmode = TRUE;
  _cursvar.cbrkmode = FALSE;
  ttysetflags();
}				/* raw */

void noraw(void)
{
  _cursvar.rawmode = FALSE;
  ttysetflags();
}				/* noraw */

void echo(void)
{
  _cursvar.echoit = TRUE;
  ttysetflags();
}

void noecho(void)
{
  _cursvar.echoit = FALSE;
  ttysetflags();
}

void nl(void)
{
  NONL = FALSE;
  ttysetflags();
}				/* nl */

void nonl(void)
{
  NONL = TRUE;
  ttysetflags();
}				/* nonl */

void cbreak(void)
{
  _cursvar.halfdelay = 0;
  _cursvar.cbrkmode = TRUE;
  _cursvar.rawmode = FALSE;
  ttysetflags();
}				/* cbreak */

void nocbreak(void)
{
  _cursvar.cbrkmode = FALSE;
  _cursvar.halfdelay = 0;
  ttysetflags();
}				/* nocbreak */

void halfdelay(int n)
{
  if (n < 1 || n > 255) return;
  _cursvar.cbrkmode = TRUE;
  _cursvar.rawmode = FALSE;
  _cursvar.halfdelay = n;
  ttysetflags();
}

