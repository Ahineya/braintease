/*
 * Copyright (c) 1993 by David I. Bell
 * Permission is granted to use, distribute, or modify this source,
 * provided that this copyright notice remains intact.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

/* Ripple adaptation: byte-at-a-time pager controls, redirected output and
 * terminal restoration. The streaming page counter follows the original. */
#include <termios.h>
#include <signal.h>
#include <sys/ioctl.h>
static int ttyfd = -1, raw;
static int rows = 24, cols = 80;
static struct termios saved;
static void restore(void) {
    if (raw) { tcsetattr(ttyfd, TCSANOW, &saved); raw = 0; }
}
static void interrupted(int sig) { restore(); _exit(128 + sig); }
static int more(FILE *fp) {
    int ch, line = 1, col = 0;
    while ((ch = fgetc(fp)) != EOF) {
        if (putchar(ch) == EOF) return -1;
        if (ch == '\r') col = 0;
        else if (ch == '\n') { line++; col = 0; }
        else if (ch == '\t') col = (col + 8) & ~7;
        else if (ch == '\b') { if (col) col--; }
        else col++;
        if (col >= cols) { line += col / cols; col %= cols; }
        if (!raw || line < rows) continue;
        fputs("--More--", stdout); fflush(stdout);
        for (;;) {
            char key;
            if (read(ttyfd, &key, 1) != 1) return 0;
            if (key == 'q' || key == 'Q') { fputs("\r        \r", stdout); return 0; }
            if (key == 'n' || key == 'N') { fputs("\r        \r", stdout); return 1; }
            if (key == ' ' || key == 'f') { line = 1; break; }
            if (key == '\r' || key == '\n') { line = rows - 1; break; }
        }
        fputs("\r        \r", stdout);
    }
    return ferror(fp) ? -1 : 1;
}
int main(int argc, char *argv[]) {
    int i, next = 1, status = 0;
    if (isatty(1)) {
        struct termios t;
        struct winsize win;
        char *name = ttyname(1), *size;
        if (!name || (ttyfd = open(name, O_RDONLY)) < 0 || tcgetattr(ttyfd, &saved) < 0) {
            perror("more: terminal"); return 1;
        }
        if (!ioctl(ttyfd, TIOCGWINSZ, &win)) {
            if (win.ws_row >= 3 && win.ws_row <= 100) rows = win.ws_row;
            if (win.ws_col >= 10 && win.ws_col <= 240) cols = win.ws_col;
        }
        size = getenv("LINES");
        if (size && atoi(size) >= 3 && atoi(size) <= 100) rows = atoi(size);
        size = getenv("COLUMNS");
        if (size && atoi(size) >= 10 && atoi(size) <= 240) cols = atoi(size);
        t = saved;
        t.c_lflag &= ~(ICANON | ECHO);
        t.c_lflag |= ISIG;
        t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
        atexit(restore); signal(SIGINT, interrupted); signal(SIGTERM, interrupted);
        if (tcsetattr(ttyfd, TCSANOW, &t) < 0) { perror("more"); return 1; }
        raw = 1;
    }
    for (i = 1; next && i < argc; i++) {
        FILE *fp = fopen(argv[i], "r");
        if (!fp) { perror(argv[i]); status = 1; continue; }
        if (raw && argc > 2) printf("<< %s >>\n", argv[i]);
        next = more(fp);
        if (next < 0) { perror("more"); status = 1; next = 1; }
        fclose(fp);
    }
    if (argc == 1 && more(stdin) < 0) status = 1;
    if (fflush(stdout) < 0) status = 1;
    return status;
}
