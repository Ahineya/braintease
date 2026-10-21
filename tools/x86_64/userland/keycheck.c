/* Interactive terminal-key probe. Uses the same curses decoder as our apps;
 * it captures input only while explicitly running in the foreground. */
#include <curses.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile int stopping;
static void stop(int sig) { (void)sig; stopping = 1; }
static const char *key_name(int key) {
    switch (key) {
    case 27: return "Escape";
    case KEY_UP: return "Up";
    case KEY_DOWN: return "Down";
    case KEY_LEFT: return "Left";
    case KEY_RIGHT: return "Right";
    case KEY_HOME: return "Home";
    case KEY_END: return "End";
    case KEY_PPAGE: return "PageUp";
    case KEY_NPAGE: return "PageDown";
    case KEY_IC: return "Insert";
    case KEY_DC: return "Delete";
    case KEY_BTAB: return "Shift-Tab";
    case KEY_BACKSPACE: case 8: case 127: return "Backspace";
    case '\t': return "Tab";
    case '\r': case '\n': return "Enter";
    default: return "Other";
    }
}
static void show_key(int row, int key) {
    move(row, 0); clrtoeol();
    if (key >= KEY_F(1) && key <= KEY_F(12))
        printw("Last key: F%d", key - KEY_F(0));
    else printw("Last key: %s", key_name(key));
}
int main(int argc, char **argv) {
    int capture = 0, keys[32], used = 0, key, i;
    unsigned functions = 0, arrows = 0;
    if (argc != 1) {
        if (argc != 3 || strcmp(argv[1], "--capture") ||
            (capture = atoi(argv[2])) < 1 || capture > 32) {
            fputs("usage: keycheck [--capture 1..32]\n", stderr); return 2;
        }
    }
    if (!initscr()) { fputs("keycheck: a terminal is required\n", stderr); return 1; }
    signal(SIGINT, stop); signal(SIGTERM, stop); signal(SIGHUP, stop);
    cbreak(); noecho(); keypad(stdscr, TRUE); timeout(100);
    curs_set(1);
    if (LINES < 8 || COLS < 40) {
        endwin(); fputs("keycheck: terminal must be at least 40 by 8\n", stderr); return 1;
    }
    clear();
    mvprintw(0, 0, "Keyboard check");
    mvprintw(1, 0, "Try Escape, arrows, and F1-F12.");
    mvprintw(2, 0, "Try Fn with F-keys if firmware needs it.");
    mvprintw(3, 0, "Press q or Ctrl-C to leave.");
    mvprintw(5, 0, "Waiting for a key...");
    refresh();
    /* Stable acceptance marker, also visible in the serial transcript. */
    mvprintw(7, 0, "KEYCHECK_READY"); refresh();
    while (!stopping) {
        key = getch();
        if (key == ERR) continue;
        if (key == 'q' || key == 3) break;
        if (capture) keys[used++] = key;
        if (key >= KEY_F(1) && key <= KEY_F(12)) functions |= 1U << (key - KEY_F(1));
        if (key == KEY_UP) arrows |= 1;
        if (key == KEY_DOWN) arrows |= 2;
        if (key == KEY_LEFT) arrows |= 4;
        if (key == KEY_RIGHT) arrows |= 8;
        show_key(5, key);
        move(6, 0); clrtoeol();
        printw("Arrows: %s  F1-F12: %s", arrows == 15 ? "all seen" : "testing",
               functions == 4095 ? "all seen" : "testing");
        move(5, 10); refresh();
        if (capture && used == capture) break;
    }
    endwin();
    for (i = 0; i < used; ++i) {
        if (keys[i] >= KEY_F(1) && keys[i] <= KEY_F(12))
            printf("KEY: F%d\n", keys[i] - KEY_F(0));
        else printf("KEY: %s\n", key_name(keys[i]));
    }
    printf("keycheck: arrows=%u/15 function-keys=%u/4095\n", arrows, functions);
    return 0;
}
