/* Interactive transport probe: actual terminal -> PTY -> curses -> event log. */
#include <curses.h>
#include <stdio.h>
int main(int argc,char **argv) {
    FILE *log;int key;MEVENT event;
    if(argc!=2||(log=fopen(argv[1],"w"))==NULL)return 1;
    if(!initscr()){fputs("INIT_FAILED\n",log);fclose(log);return 1;}
    raw();noecho();keypad(stdscr,TRUE);wtimeout(stdscr,100);
    ripple_curses_mouse_mode(3);
    mvaddstr(0,0,"Select this terminal text");
    mvaddstr(2,0,"Mouse transport probe: click, drag, wheel; q quits");refresh();
    fputs("READY\n",log);fflush(log);
    while((key=getch())!='q')if(key==KEY_MOUSE&&getmouse(&event)==OK) {
        fprintf(log,"M %d %d %lu\n",event.x,event.y,event.bstate);fflush(log);
    }
    endwin();fputs("CLOSED\n",log);fclose(log);return 0;
}
