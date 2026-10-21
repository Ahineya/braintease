/* Exercise the actual C16 library on a private guest PTY, on both kernels. */
#include <curses.h>
#include "../../../libraries/tui-theme/tui_curses.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
/* Exercise the same bounded drawing adapter used by RFL applications. */
#include "../../../rfl/os/curses.c"

static int report, failures, assertions;
static volatile sig_atomic_t alarm_seen;
static void interrupted(int sig) { (void)sig; alarm_seen = 1; }
static void check(int yes) {
    ++assertions;
    if (!yes) ++failures;
    write(report, yes ? "Y" : "N", 1);
}
static void scan_checks(void) {
    int a = 0, b = 0, consumed = -1;
    unsigned u;
    long l;
    unsigned long ul;
    char text[8] = {0}, pair[2] = {0};
    check(sscanf("-32768 32767", "%d %d", &a, &b) == 2 && a == INT_MIN && b == INT_MAX);
    check(sscanf("65535", "%u", &u) == 1 && u == UINT_MAX);
    check(sscanf("-2147483648", "%ld", &l) == 1 && l == LONG_MIN);
    check(sscanf("4294967295", "%lu", &ul) == 1 && ul == ULONG_MAX);
    check(sscanf("0x10 077", "%i %i%n", &a, &b, &consumed) == 2 && a == 16 && b == 63 && consumed == 8);
    check(sscanf("123 skip abcd", "%*d %*s %3s", text) == 1 && !strcmp(text, "abc"));
    check(sscanf("abcd123", "%4[a-z]%d", text, &a) == 2 && !strcmp(text, "abcd") && a == 123);
    check(sscanf("ab]", "%2[^]]", text) == 1 && !strcmp(text, "ab"));
    check(sscanf("]-z", "%2[]-]", text) == 1 && !strcmp(text, "]-"));
    check(sscanf("xy", "%2c", pair) == 1 && pair[0] == 'x' && pair[1] == 'y');
    check(sscanf("", "%d", &a) == EOF && sscanf("no", "%d", &a) == 0);
    a = 99; errno = 0;
    check(sscanf("32768", "%d", &a) == 0 && a == 99 && errno == ERANGE);
    errno = 0;
    check(sscanf("1.5", "%f", &a) == 0 && errno == EINVAL && a == 99);
}
static void mouse_checks(int master) {
    MEVENT mouse;mmask_t old;uint32_t value,data[3];int x=1,y=1;
    WINDOW *w=newwin(2,3,1,2);
    check(w!=NULL);if(!w)return;
    check(mousemask(ALL_MOUSE_EVENTS|REPORT_MOUSE_POSITION,&old)==(ALL_MOUSE_EVENTS|REPORT_MOUSE_POSITION)&&old==0);
    check(write(master,"\033[<0;4;3M",9)==9&&getch()==KEY_MOUSE&&getmouse(&mouse)==OK);
    check(mouse.x==3&&mouse.y==2&&mouse.bstate==BUTTON1_PRESSED&&getmouse(&mouse)==ERR);
    check(wenclose(w,2,3)&&!wenclose(w,3,3)&&wmouse_trafo(w,&y,&x,TRUE)&&y==2&&x==3);
    check(wmouse_trafo(w,&y,&x,FALSE)&&y==1&&x==1);
    check(write(master,"\033[<48;4;3M",10)==10&&wget_wch(stdscr,&value)==KEY_CODE_YES&&value==KEY_MOUSE);
    check(getmouse(&mouse)==OK&&mouse.bstate==(BUTTON1_PRESSED|BUTTON_CTRL|REPORT_MOUSE_POSITION));
    check(write(master,"\033[<0;4;3m",9)==9&&getch()==KEY_MOUSE&&getmouse(&mouse)==OK&&mouse.bstate==BUTTON1_RELEASED);
    check(write(master,"\033[<64;4;3M",10)==10&&getch()==KEY_MOUSE&&getmouse(&mouse)==OK&&mouse.bstate==BUTTON4_PRESSED);
    check(write(master,"\033[<65;4;3M",10)==10&&getch()==KEY_MOUSE&&rfl_curses_getmouse(data)==0&&data[0]==3&&data[1]==2&&data[2]==BUTTON5_PRESSED);
    check(write(master,"\033[<35;4;3M",10)==10&&getch()==KEY_MOUSE&&getmouse(&mouse)==OK&&mouse.bstate==REPORT_MOUSE_POSITION);
    check(mousemask(0,&old)==0&&old==(ALL_MOUSE_EVENTS|REPORT_MOUSE_POSITION));
    check(write(master,"\033[<0;4;3MX",10)==10&&getch()=='X');
    check(delwin(w)==OK);
}
static void unicode_checks(int master) {
    uint32_t value;WINDOW *w=newwin(2,6,0,0);
    static const unsigned char title[]={0xd0,0x96,'e',0xcc,0x81,0};
    static const char lead[]={0xc4},tail[]={0x82},bad[]={0xe2,27,'[','A'};
    static const char line[]={0xd0,0x96,'e',0xcc,0x81,127,'\n'};
    static const char collision[]={0xc4,0x87,'\n'};
    char text[16];
    check(w!=NULL);if(!w)return;
    wattrset(w,A_BOLD|COLOR_PAIR(5));wmove(w,0,0);
    check(ripple_curses_scalar(w,'A')==OK&&ripple_curses_scalar(w,0x4e2d)==OK&&ripple_curses_scalar(w,0x1f642UL)==OK);
    check((w->_line[0][1]&A_CHARTEXT)==0x4e2d&&(w->_line[0][2]&A_CHARTEXT)==RU_CONTINUATION&&
          (w->_line[0][3]&A_CHARTEXT)==0x1f642UL&&PAIR_NUMBER(w->_line[0][3])==5);
    wmove(w,1,0);check(wadd_utf8(w,title,5)==OK);
    check((w->_line[1][0]&A_CHARTEXT)==0x416&&ru_scalar(w->_line[1][1]&A_CHARTEXT,1)==0x301&&w->_curx==2);
    wmove(w,0,2);ripple_curses_scalar(w,'X');check((w->_line[0][1]&A_CHARTEXT)==' '&&(w->_line[0][2]&A_CHARTEXT)=='X');
    wbkgd(w,0);wmove(w,0,3);waddch(w,'Z');
    rfl_curses_fill(w,0,0,3,0x4e2d,A_BOLD);
    check((w->_line[0][0]&A_CHARTEXT)==0x4e2d&&(w->_line[0][1]&A_CHARTEXT)==RU_CONTINUATION&&
          (w->_line[0][2]&A_CHARTEXT)==' '&&(w->_line[0][3]&A_CHARTEXT)=='Z');
    rfl_curses_fill(w,1,5,300,0x4e2d,0);
    check((w->_line[1][5]&A_CHARTEXT)==' '&&w->_cury==1);
    rfl_curses_fill(w,1,0,2,0x301,0);
    check((w->_line[1][0]&A_CHARTEXT)==RU_REPLACEMENT&&(w->_line[1][1]&A_CHARTEXT)==RU_REPLACEMENT);
    wmove(w,1,4);ripple_curses_scalar(w,0x80000000UL);ripple_curses_scalar(w,0xd800);
    check(w->_line[1][4]==RU_REPLACEMENT&&w->_line[1][5]==RU_REPLACEMENT);
    check(delwin(w)==OK);
    check(write(master,lead,1)==1&&wget_wch(stdscr,&value)==ERR);
    check(write(master,tail,1)==1&&wget_wch(stdscr,&value)==OK&&value==0x102);
    check(write(master,"\033[B",3)==3&&wget_wch(stdscr,&value)==KEY_CODE_YES&&value==KEY_DOWN);
    check(write(master,bad,sizeof(bad))==sizeof(bad)&&wget_wch(stdscr,&value)==OK&&value==RU_REPLACEMENT);
    check(wget_wch(stdscr,&value)==KEY_CODE_YES&&value==KEY_UP);
    check(write(master,line,sizeof(line))==sizeof(line)&&wgetnstr(stdscr,text,15)==OK&&text[0]==0xd0&&text[1]==0x96&&!text[2]);
    check(write(master,collision,sizeof(collision))==sizeof(collision)&&wgetnstr(stdscr,text,15)==OK&&text[0]==0xc4&&text[1]==0x87&&!text[2]);
}
int main(void) {
    int master, slave, original_in, original_out, n, i;
    int pid, status;
    struct termios original, current;
    struct winsize size = {8, 20, 0, 0};
    WINDOW *cached, *parent, *child;
    char output[256];
    struct { char before; char text[4]; char after; } bounded = {'L', {0}, 'R'};
    const char keys[] = "\033[A\033OB\033[C\033[D\033[H\033[F\033[2~\033[3~\033[5~\033[6~\033OR\033[15~\033[21~";
    const int expected[] = {KEY_UP, KEY_DOWN, KEY_RIGHT, KEY_LEFT, KEY_HOME, KEY_END, KEY_IC, KEY_DC, KEY_PPAGE, KEY_NPAGE, KEY_F(3), KEY_F(5), KEY_F(10)};
    report = dup(1); if (report < 0) return 2;
    scan_checks();
    {
        FILE *temporary = tmpfile();
        char data[8] = {0};
        check(temporary != NULL);
        if (temporary) {
            check(fputs("C16", temporary) >= 0 && fseek(temporary, 0, SEEK_SET) == 0 &&
                  fread(data, 1, 3, temporary) == 3 && !strcmp(data, "C16"));
            check(fclose(temporary) == 0);
        }
    }
    master = open("/dev/ptyp0", O_RDWR | O_NONBLOCK | O_NOCTTY);
    slave = open("/dev/ttyp0", O_RDWR | O_NOCTTY);
    if (master < 0 || slave < 0) return 3;
    if (tcgetattr(slave, &original) < 0 || ioctl(slave, TIOCSWINSZ, &size) < 0) return 4;
    original_in = dup(0); original_out = dup(1);
    fflush(stdout);
    if (dup2(slave, 0) < 0 || dup2(slave, 1) < 0) return 5;
    cached = initscr();
    check(cached != NULL && LINES == 8 && COLS == 20);
    if (!cached) return 6;
    { int rows, cols; getmaxyx(stdscr, rows, cols); check(rows == 8 && cols == 20); }
    {
        TuiTheme *theme = tui_theme_new("C app", 1);
        chtype styles[1], fallback[1] = {A_REVERSE};
        check(theme != NULL && tui_theme_define(theme, "custom", 3, 5, TUI_BOLD) == 0);
        check(tui_curses_install(theme, 6, styles, fallback) == OK &&
              styles[0] == (A_BOLD | COLOR_PAIR(6)));
        check(tui_curses_install(theme, 64, styles, fallback) == ERR);
        tui_theme_free(theme);
    }
    raw(); noecho(); keypad(stdscr, TRUE); nodelay(stdscr, TRUE);
    check(tcgetattr(0, &current) == 0 && !(current.c_lflag & (ECHO | ICANON | ISIG)));
    check(getch() == ERR);
    while (read(master, output, sizeof(output)) > 0) {}
    check(getch() == ERR);
    check(read(master, output, sizeof(output)) < 0 && errno == EAGAIN);
    move(1, 2); check(getch() == ERR && curscr->_cury == 1 && curscr->_curx == 2);
    check(write(master, keys, sizeof(keys) - 1) == sizeof(keys) - 1);
    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) check(getch() == expected[i]);
    check(ungetch('q') == OK && getch() == 'q');
    check(ungetc('b', stdin) == 'b' && getch() == 'b');
    check(write(master, "\033[99~", 5) == 5 && getch() == 27 && getch() == '[' && getch() == '9' && getch() == '9' && getch() == '~');
    check(write(master, "\033", 1) == 1 && getch() == 27);
    keypad(stdscr, FALSE);
    check(write(master, "\033[A", 3) == 3 && getch() == 27 && getch() == '[' && getch() == 'A');
    keypad(stdscr, TRUE);
    unicode_checks(master);
    mouse_checks(master);
    check(write(master, "abcd\177Z\n", 7) == 7 && wgetnstr(stdscr, bounded.text, 3) == OK &&
          !strcmp(bounded.text, "abZ") && bounded.before == 'L' && bounded.after == 'R');
    check(wgetstr(stdscr, bounded.text) == ERR);
    wtimeout(stdscr, 20); check(getch() == ERR); nodelay(stdscr, TRUE);
    halfdelay(1); nodelay(stdscr, FALSE); check(getch() == ERR);
    raw(); nodelay(stdscr, TRUE);
    pid = fork();
    if (!pid) {
        write(master, "\033", 1); usleep(50000UL); write(master, "[A", 2); _exit(0);
    }
    nodelay(stdscr, FALSE);
    check(pid > 0 && getch() == KEY_UP);
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    signal(SIGALRM, interrupted); alarm(1);
    check(getch() == ERR && alarm_seen && errno == EINTR);
    alarm(0); signal(SIGALRM, SIG_DFL); nodelay(stdscr, TRUE);
    check(newwin(-1, 2, 0, 0) == NULL && newwin(9, 2, 0, 0) == NULL);
    parent = newwin(4, 5, 1, 1); child = subwin(parent, 0, 0, 3, 4);
    check(parent != NULL && child != NULL && child->_maxy == 1 && child->_maxx == 1);
    check(mvwin(parent, -1, 0) == ERR && mvwin(child, 0, 0) == ERR);
    check(delwin(parent) == ERR && delwin(child) == OK && delwin(parent) == OK);
    {
        int fg = 0, bg = 0;
        /* One C16 character is 16 bits; a 32-bit cell occupies two. */
        check(sizeof(chtype) == 2 && start_color() == OK && COLORS == 256 && COLOR_PAIRS == 64);
        check(use_default_colors() == OK && init_pair(5, COLOR_WHITE, COLOR_BLUE) == OK);
        check(pair_content(5, &fg, &bg) == OK && fg == COLOR_WHITE && bg == COLOR_BLUE);
        check(init_pair(0, 1, 2) == ERR && init_pair(64, 1, 2) == ERR && init_pair(1, 256, 0) == ERR);
        check(init_pair(6, -1, 255) == OK && PAIR_NUMBER(COLOR_PAIR(63) | A_BOLD) == 63);
        {
            chtype cells[3] = {'A' | COLOR_PAIR(5), 'B' | A_BOLD, '\n' | A_REVERSE};
            parent = newwin(2, 3, 1, 1);
            check(parent && ripple_curses_cells(parent, 1, 0, cells, 3) == OK);
            check(parent->_line[1][0] == cells[0] && parent->_line[1][1] == cells[1] &&
                  parent->_line[1][2] == ('?' | A_REVERSE));
            check((parent->_line[0][0] & A_CHARTEXT) == ' ' && parent->_cury == 1 && parent->_curx == 2);
            check(ripple_curses_cells(parent, 0, 2, cells, 3) == OK && parent->_line[0][2] == cells[0]);
            check(ripple_curses_cells(parent, -1, 0, cells, 1) == ERR &&
                  ripple_curses_cells(parent, 0, 3, cells, 1) == ERR &&
                  ripple_curses_cells(parent, 0, 0, NULL, 1) == ERR);
            check(ripple_curses_cursor_shape(-1) == ERR && ripple_curses_cursor_shape(4) == ERR);
            check(ripple_curses_cursor_shape(1) == OK && ripple_curses_cursor_shape(3) == OK &&
                  ripple_curses_cursor_shape(0) == OK);
            check(delwin(parent) == OK);
        }
        wbkgd(stdscr, COLOR_PAIR(5));
        wattrset(stdscr, COLOR_PAIR(5) | A_REVERSE);
        mvaddch(1, 1, 'S'); move(1, 1);
        check((winch(stdscr) & (A_COLOR | A_REVERSE | A_CHARTEXT)) == (COLOR_PAIR(5) | A_REVERSE | 'S'));
        ripple_curses_lines(1); box(stdscr, 0, 0);
        check((stdscr->_line[0][0] & (A_ALTCHARSET | A_CHARTEXT)) == (A_ALTCHARSET | 'l'));
        /* This probe owns both ends of a bounded guest PTY. Drain before
         * rendering, and test the frame's cells separately from full output. */
        while (read(master, output, sizeof(output)) > 0) {}
        wbkgd(stdscr, COLOR_PAIR(5));
        parent = newwin(3, 6, 2, 3);
        check(parent && wbkgd(parent, COLOR_PAIR(5)) == OK);
        mvwaddstr(parent, 0, 0, "first"); mvwaddstr(parent, 1, 0, "second");
        scrollok(parent, TRUE); check(wscrl(parent, 1) == OK && (parent->_line[0][0] & A_CHARTEXT) == 's');
        check(wscrl(parent, -1) == OK && parent->_line[0][0] == (' ' | COLOR_PAIR(5)) && (parent->_line[1][0] & A_CHARTEXT) == 's');
        wbkgd(parent, COLOR_PAIR(5)); mvwaddch(parent, 1, 1, 'P');
        wnoutrefresh(stdscr); wnoutrefresh(parent); doupdate();
        check((curscr->_line[3][4] & A_CHARTEXT) == 'P');
        while (read(master, output, sizeof(output)) > 0) {}
        check(mvwin(parent, 1, 10) == OK);
        wnoutrefresh(stdscr); wnoutrefresh(parent); doupdate();
        check((curscr->_line[2][11] & A_CHARTEXT) == 'P' && (curscr->_line[3][4] & A_CHARTEXT) != 'P');
        while (read(master, output, sizeof(output)) > 0) {}
        check(delwin(parent) == OK);
        wnoutrefresh(stdscr); doupdate();
        check((curscr->_line[2][11] & A_CHARTEXT) != 'P');
        while (read(master, output, sizeof(output)) > 0) {}
        parent = subwin(stdscr, 2, 3, 1, 1);
        scrollok(parent, TRUE); mvwaddch(parent, 1, 0, 'D');
        check(wscrl(parent, 1) == OK && (stdscr->_line[1][1] & A_CHARTEXT) == 'D');
        check(delwin(parent) == OK);
        ripple_curses_lines(0); wbkgd(stdscr, A_NORMAL);
    }
    wattrset(stdscr, A_BOLD); mvaddstr(0, 0, "BOLD"); refresh();
    i = 1;
    while ((n = read(master, output, sizeof(output))) > 0) {
        int j; for (j = 0; j < n; ++j) if ((unsigned char)output[j] > 127) i = 0;
    }
    check(i);
    parent = newwin(3, 4, 5, 16);
    check(parent != NULL);
    mvwaddstr(parent, 0, 0, "XYZW");
    size.ws_row = 6; size.ws_col = 18;
    check(ioctl(slave, TIOCSWINSZ, &size) == 0 && getch() == KEY_RESIZE && stdscr == cached && LINES == 6 && COLS == 18);
    wnoutrefresh(parent); doupdate();
    check((curscr->_line[5][16] & A_CHARTEXT) == 'X' && (curscr->_line[5][17] & A_CHARTEXT) == 'Y' && delwin(parent) == OK);
    while (read(master, output, sizeof(output)) > 0) {}
    check((stdscr->_line[0][0] & A_CHARTEXT) == 'B' && getch() == ERR);
    check(endwin() == OK && endwin() == OK && isendwin());
    check(tcgetattr(0, &current) == 0 && current.c_lflag == original.c_lflag && current.c_iflag == original.c_iflag);
    check(reset_prog_mode() == OK && !isendwin() && stdscr == cached);
    check(tcgetattr(0, &current) == 0 && !(current.c_lflag & (ICANON | ISIG)));
    fflush(stdout);
    pid = fork();
    if (!pid) {
        endwin(); kill(getpid(), SIGSTOP);
        if (reset_prog_mode() == ERR) _exit(7);
        /* The registered atexit hook, not a manual endwin, restores input. */
        exit(42);
    }
    check(pid > 0 && waitpid(pid, &status, WUNTRACED) == pid && WIFSTOPPED(status));
    check(tcgetattr(0, &current) == 0 && current.c_lflag == original.c_lflag);
    check(kill(pid, SIGCONT) == 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 42);
    check(tcgetattr(0, &current) == 0 && current.c_lflag == original.c_lflag);
    check(reset_prog_mode() == OK);
    check(endwin() == OK);
    close(master);
    check(getch() == ERR);
    fflush(stdout); dup2(original_in, 0); dup2(original_out, 1);
    close(original_in); close(original_out); close(slave);
    write(report, "\n", 1);
    printf("CURSES_%s %d checks\n", failures ? "FAILED" : "OK", assertions);
    close(report);
    return failures ? 1 : 0;
}
