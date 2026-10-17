/* curses.h - defines macros and prototypes for curses */

#ifndef CURSES_H

#include <termios.h>
#include <stdarg.h>
#include <stdio.h>

/* C23 this became a real type */
#if ( __STDC_VERSION__ < 202000)
typedef int bool;
#endif

#define TRUE 1
#define FALSE 0
#define ERR -1		/* general error flag */
#define OK 0		/* general OK flag */

/* Macros. */
#define box(win,vc,hc) wbox(win,0,0,0,0,vc,hc)
#define addch(ch) waddch(stdscr,ch)
#define mvaddch(y,x,ch) (wmove(stdscr,y,x)==ERR?ERR:waddch(stdscr,ch))
#define mvwaddch(win,y,x,ch) (wmove(win,y,x)==ERR?ERR:waddch(win,ch))
#define getch() wgetch(stdscr)
#define mvgetch(y,x) (wmove(stdscr,y,x)==ERR?ERR:wgetch(stdscr))
#define mvwgetch(win,y,x) (wmove(win,y,x)==ERR?ERR:wgetch(win))
#define addstr(str) waddstr(stdscr,str)
#define mvaddstr(y,x,str) (wmove(stdscr,y,x)==ERR?ERR:waddstr(stdscr,str))
#define mvwaddstr(win,y,x,str) (wmove(win,y,x)==ERR?ERR:waddstr(win,str))
#define getstr(str) wgetstr(stdscr,str)
#define mvgetstr(y,x,str) (wmove(stdscr,y,x)==ERR?ERR:wgetstr(stdscr,str))
#define mvwgetstr(win,y,x,str) (wmove(win,y,x)==ERR?ERR:wgetstr(win,str))
#define move(y,x) wmove(stdscr,y,x)
#define clear() wclear(stdscr)
#define erase() werase(stdscr)
#define clrtobot() wclrtobot(stdscr)
#define mvclrtobot(y,x) (wmove(stdscr,y,x)==ERR?ERR:wclrtobot(stdscr))
#define mvwclrtobot(win,y,x) (wmove(win,y,x)==ERR?ERR:wclrtobot(win))
#define clrtoeol() wclrtoeol(stdscr)
#define mvclrtoeol(y,x) (wmove(stdscr,y,x)==ERR?ERR:wclrtoeol(stdscr))
#define mvwclrtoeol(win,y,x) (wmove(win,y,x)==ERR?ERR:wclrtoeol(win))
#define insertln() winsertln(stdscr)
#define mvinsertln(y,x) (wmove(stdscr,y,x)==ERR?ERR:winsertln(stdscr))
#define mvwinsertln(win,y,x) (wmove(win,y,x)==ERR?ERR:winsertln(win))
#define deleteln() wdeleteln(stdscr)
#define mvdeleteln(y,x) (wmove(stdscr,y,x)==ERR?ERR:wdeleteln(stdscr))
#define mvwdeleteln(win,y,x) (wmove(win,y,x)==ERR?ERR:wdeleteln(win))
#define refresh() wrefresh(stdscr)
#define inch() winch(stdscr)
#define insch(ch) winsch(stdscr,ch)
#define mvinsch(y,x,ch) (wmove(stdscr,y,x)==ERR?ERR:winsch(stdscr,ch))
#define mvwinsch(win,y,x,ch) (wmove(win,y,x)==ERR?ERR:winsch(win,ch))
#define delch() wdelch(stdscr)
#define mvdelch(y,x) (wmove(stdscr,y,x)==ERR?ERR:wdelch(stdscr))
#define mvwdelch(win,y,x) (wmove(win,y,x)==ERR?ERR:wdelch(win))
#define standout() wstandout(stdscr)
#define wstandout(win) (win)->_attrs |= A_STANDOUT
#define standend() wstandend(stdscr)
#define wstandend(win) (win)->_attrs &= ~A_STANDOUT
#define attrset(attrs) wattrset(stdscr, attrs)
#define wattrset(win, attrs) (win)->_attrs = (attrs)
#define attron(attrs) wattron(stdscr, attrs)
#define wattron(win, attrs) (win)->_attrs |= (attrs)
#define attroff(attrs) wattroff(stdscr,attrs)
#define wattroff(win, attrs) (win)->_attrs &= ~(attrs)
#define resetty() stty(1, &_orig_tty)
#define getyx(win,y,x) (y = (win)->_cury, x = (win)->_curx)
#define getmaxyx(win,y,x) (y = (win)->_maxy + 1, x = (win)->_maxx + 1)
#define getbegyx(win,y,x) (y = (win)->_begy, x = (win)->_begx)

/* Video attribute definitions. */
#define A_BLINK        0x0100
#define A_BLANK        0
#define A_BOLD	       0x0200
#define A_DIM	       0
#define A_PROTECT      0
#define A_REVERSE      0x0400
#define A_STANDOUT     0x0800
#define A_UNDERLINE    0x1000
#define A_ALTCHARSET   0x2000

/* 32-bit cells retain characters, legacy attributes and 64 independent color pairs. */
typedef unsigned long chtype;

/* Type declarations. */
typedef struct ripple_curses_window {
  int	   _cury;			/* current pseudo-cursor */
  int	   _curx;
  int	   _maxy;			/* max coordinates */
  int	   _maxx;
  int	   _begy;			/* origin on screen */
  int	   _begx;
  int	   _flags;			/* window properties */
  chtype	   _attrs;			/* attributes of written characters */
  int        _delay; /* wgetch timeout in ms: -1 blocking, 0 poll. */
  struct ripple_curses_window *_parent;
  int        _children;
  int	   _tabsize;			/* tab character size */
  bool	   _clear;			/* causes clear at next refresh */
  bool	   _leave;			/* leaves cursor as it happens */
  bool	   _scroll;			/* allows window scrolling */
  bool	   _nodelay;			/* input character wait flag */
  bool	   _keypad;			/* flags keypad key mode active */
  chtype	 **_line;			/* pointer to line pointer array */
  int	  *_minchng;			/* First changed character in line */
  int	  *_maxchng;			/* Last changed character in line */
  int	   _regtop;			/* Top/bottom of scrolling region */
  int	   _regbottom;
} WINDOW;

/* External variables */
extern int LINES;			/* terminal height */
extern int COLS;			/* terminal width */
extern bool NONL;			/* \n causes CR too ? */
extern WINDOW *curscr;			/* the current screen image */
extern WINDOW *stdscr;			/* the default screen window */
extern struct termios _orig_tty, _tty;

extern unsigned int ACS_ULCORNER;	/* terminal dependent block grafic */
extern unsigned int ACS_LLCORNER;	/* charcters.  Forget IBM, we are */
extern unsigned int ACS_URCORNER;	/* independent of their charset. :-) */
extern unsigned int ACS_LRCORNER;
extern unsigned int ACS_RTEE;
extern unsigned int ACS_LTEE;
extern unsigned int ACS_BTEE;
extern unsigned int ACS_TTEE;
extern unsigned int ACS_HLINE;
extern unsigned int ACS_VLINE;
extern unsigned int ACS_PLUS;
extern unsigned int ACS_S1;
extern unsigned int ACS_S9;
extern unsigned int ACS_DIAMOND;
extern unsigned int ACS_CKBOARD;
extern unsigned int ACS_DEGREE;
extern unsigned int ACS_PLMINUS;
extern unsigned int ACS_BULLET;
extern unsigned int ACS_LARROW;
extern unsigned int ACS_RARROW;
extern unsigned int ACS_DARROW;
extern unsigned int ACS_UARROW;
extern unsigned int ACS_BOARD;
extern unsigned int ACS_LANTERN;
extern unsigned int ACS_BLOCK;

extern char *unctrl(char _c);
extern int baudrate(void);
extern void beep(void);
extern void cbreak(void);
extern void clearok(WINDOW *_win, bool _flag);
extern void clrscr(void);
extern void curs_set(int _visibility);
extern int delwin(WINDOW *_win);
extern void doupdate(void);
extern void echo(void);
extern int endwin(void);
extern int erasechar(void);
extern void fatal(char *_s);
extern int fixterm(void);
extern void flash(void);
extern void gettmode(void);
extern void halfdelay(int _flag);
extern void idlok(WINDOW *_win, bool _flag);
extern WINDOW *initscr(void);
extern void keypad(WINDOW *_win, bool _flag);
extern int killchar(void);
extern void leaveok(WINDOW *_win, bool _flag);
extern char *longname(void);
extern void meta(WINDOW *_win, bool _flag);
extern int mvcur(int _oldy, int _oldx, int _newy, int _newx);
extern chtype mvinch(int _y, int _x);
extern int mvprintw(int _y, int _x, const char *_fmt, ...);
extern int mvscanw(int _y, int _x, const char *_fmt, ...);
extern int mvwin(WINDOW *_win, int _begy, int _begx);
extern chtype mvwinch(WINDOW *_win, int _y, int _x);
extern int mvwprintw(WINDOW *_win, int _y, int _x, const char *_fmt, ...);
extern int mvwscanw(WINDOW *_win, int _y, int _x, const char *_fmt,
                                                                        ...);
extern WINDOW *newwin(int _num_lines, int _num_cols, int _y, int _x);
extern void nl(void);
extern void nocbreak(void);
extern void nodelay(WINDOW *_win, bool _flag);
extern void noecho(void);
extern void nonl(void);
extern void noraw(void);
extern int outc(int _c);
extern void overlay(WINDOW *_win1, WINDOW *_win2);
extern void overwrite(WINDOW *_win1, WINDOW *_win2);
extern void poscur(int _r, int _c);
extern int printw(const char *_fmt, ...);
extern void raw(void);
extern int resetterm(void);
extern int saveoldterm(void);
extern int saveterm(void);
extern int savetty(void);
extern int scanw(const char *_fmt, ...);
extern void scroll(WINDOW *_win);
extern void scrollok(WINDOW *_win, bool _flag);
extern int setscrreg(int _top, int _bottom);
extern int setterm(char *_type);
extern int setupterm(void);
extern WINDOW *subwin(WINDOW *_orig, int _nlines, int _ncols, int _y,
					int _x);
extern int tabsize(int _ts);
extern void touchwin(WINDOW *_win);
extern int waddch(WINDOW *_win, chtype _c);
extern int waddstr(WINDOW *_win, char *_str);
extern int wbox(WINDOW *_win, int _ymin, int _xmin, int _ymax,
				int _xmax, unsigned int _v, unsigned int _h);
extern void wclear(WINDOW *_win);
extern int wclrtobot(WINDOW *_win);
extern int wclrtoeol(WINDOW *_win);
extern int wdelch(WINDOW *_win);
extern int wdeleteln(WINDOW *_win);
extern void werase(WINDOW *_win);
extern int wgetch(WINDOW *_win);
extern int wgetstr(WINDOW *_win, char *_str);
extern chtype winch(WINDOW *_win);
extern int winsch(WINDOW *_win, char _c);
extern int winsertln(WINDOW *_win);
extern int wmove(WINDOW *_win, int _y, int _x);
extern void wnoutrefresh(WINDOW *_win);
extern int wprintw(WINDOW *_win, const char *_fmt, ...);
extern void wrefresh(WINDOW *_win);
extern int wscanw(WINDOW *_win, const char *_fmt, ...);
extern int wsetscrreg(WINDOW *_win, int _top, int _bottom);
extern int wtabsize(WINDOW *_win, int _ts);

/* Characters and attributes are separate from decoded input key codes. */
#define A_NORMAL 0
#define A_COLOR 0x003f0000UL
#define A_ATTRIBUTES 0x003fff00UL
#define COLOR_PAIR(n) (((chtype)(n) & 63UL) << 16)
#define PAIR_NUMBER(a) ((int)(((chtype)(a) >> 16) & 63UL))
#define COLOR_BLACK 0
#define COLOR_RED 1
#define COLOR_GREEN 2
#define COLOR_YELLOW 3
#define COLOR_BLUE 4
#define COLOR_MAGENTA 5
#define COLOR_CYAN 6
#define COLOR_WHITE 7
extern int COLORS, COLOR_PAIRS;
int has_colors(void);
int start_color(void);
int use_default_colors(void);
int init_pair(int, int, int);
int pair_content(int, int *, int *);
void ripple_curses_lines(int);
void ripple_curses_invalidate(void);
int wscrl(WINDOW *, int);
int wbkgd(WINDOW *, chtype);
#define A_CHARTEXT 0x00ffU
#define KEY_MIN 0x101
#define KEY_DOWN 0x102
#define KEY_UP 0x103
#define KEY_LEFT 0x104
#define KEY_RIGHT 0x105
#define KEY_HOME 0x106
#define KEY_BACKSPACE 0x107
#define KEY_F0 0x108
#define KEY_F(n) (KEY_F0 + (n))
#define KEY_DC 0x14a
#define KEY_IC 0x14b
#define KEY_NPAGE 0x152
#define KEY_PPAGE 0x153
#define KEY_END 0x168
#define KEY_BTAB 0x161
#define KEY_CLEAR 0515
#define KEY_EOS 0516
#define KEY_EOL 0517
#define KEY_ENTER 0527
#define KEY_LL 0533
#define KEY_A1 0534
#define KEY_A3 0535
#define KEY_B2 0536
#define KEY_C1 0537
#define KEY_C3 0540
#define KEY_FIND 0552
#define KEY_HELP 0553
#define KEY_REDO 0563
#define KEY_SELECT 0601
#define KEY_RESIZE 0x19a
#define getnstr(s,n) wgetnstr(stdscr,s,n)
#define timeout(n) wtimeout(stdscr,n)
int wgetnstr(WINDOW *, char *, int);
int ungetch(int);
void wtimeout(WINDOW *, int);
int isendwin(void);
int def_prog_mode(void);
int reset_prog_mode(void);
int def_shell_mode(void);
int reset_shell_mode(void);
int resizeterm(int, int);
int resize_term(int, int);
/* Returns KEY_RESIZE once after a changed terminal size, or zero. */
int ripple_curses_resize(void);
/* Packed cell writes; shape 0 default, 1 block, 2 underline, 3 bar. */
int ripple_curses_cells(WINDOW *, int, int, const chtype *, int);
int ripple_curses_cursor_shape(int);
#define CURSES_H

#endif
