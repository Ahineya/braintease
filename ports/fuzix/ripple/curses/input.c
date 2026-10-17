/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bounded, select-driven ASCII input for the shared C16 curses port. */
#include <curses.h>
#include <errno.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include "curspriv.h"

static int pushed[64], count;
static int milliseconds(unsigned long *value) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return ERR;
    *value = (unsigned long)now.tv_sec * 1000UL + now.tv_nsec / 1000000L;
    return OK;
}
int ungetch(int c) {
    if (c == ERR || count == 64) return ERR;
    pushed[count++] = c;
    return OK;
}
void wtimeout(WINDOW *w, int milliseconds) {
    w->_delay = milliseconds < 0 ? -1 : milliseconds;
    w->_nodelay = milliseconds == 0;
}
/* Consume FILE lookahead before selecting the underlying descriptor. Mixing
 * stdio input and readiness must not strand characters in stdin's buffer. */
static int input(int delay) {
    fd_set readset;
    struct timeval timeout, *wait = NULL;
    char c;
    int fd = fileno(stdin), n;
    if (stdin->pushed != EOF) { n = stdin->pushed; stdin->pushed = EOF; return n; }
    if (stdin->pos < stdin->len) return (unsigned char)stdin->buf[stdin->pos++];
    if (fd < 0 || fd >= FD_SETSIZE) { errno = EBADF; return ERR; }
    FD_ZERO(&readset); FD_SET(fd, &readset);
    if (delay >= 0) {
        timeout.tv_sec = (unsigned)delay / 1000;
        timeout.tv_usec = ((unsigned long)delay % 1000) * 1000;
        wait = &timeout;
    }
    n = select(fd + 1, &readset, NULL, NULL, wait);
    if (n <= 0) return ERR;
    n = read(fd, &c, 1);
    if (n != 1) { if (!n) stdin->eof = 1; return ERR; }
    return (unsigned char)c;
}
static int sequence(const int *s, int n) {
    int last = s[n - 1], number = 0, i;
    if (s[1] != '[' && s[1] != 'O') return ERR;
    if (last == 'A') return KEY_UP;
    if (last == 'B') return KEY_DOWN;
    if (last == 'C') return KEY_RIGHT;
    if (last == 'D') return KEY_LEFT;
    if (last == 'H') return KEY_HOME;
    if (last == 'F') return KEY_END;
    if (last == 'Z') return KEY_BTAB;
    if (s[1] == 'O' && n == 3 && last >= 'P' && last <= 'S') return KEY_F(last - 'P' + 1);
    if (last != '~') return ERR;
    for (i = 2; i < n - 1 && s[i] >= '0' && s[i] <= '9'; ++i) {
        if (number > 100) return ERR;
        number = number * 10 + s[i] - '0';
    }
    switch (number) {
    case 1: case 7: return KEY_HOME;
    case 2: return KEY_IC;
    case 3: return KEY_DC;
    case 4: case 8: return KEY_END;
    case 5: return KEY_PPAGE;
    case 6: return KEY_NPAGE;
    case 11: return KEY_F(1);
    case 12: return KEY_F(2);
    case 13: return KEY_F(3);
    case 14: return KEY_F(4);
    case 15: return KEY_F(5);
    case 17: return KEY_F(6);
    case 18: return KEY_F(7);
    case 19: return KEY_F(8);
    case 20: return KEY_F(9);
    case 21: return KEY_F(10);
    case 23: return KEY_F(11);
    case 24: return KEY_F(12);
    default: return ERR;
    }
}
int wgetch(WINDOW *w) {
    int c, s[32], n = 1, key;
    unsigned long start, now;
    if (!w || !stdscr) { errno = EINVAL; return ERR; }
    if (ripple_curses_resize()) return KEY_RESIZE;
    if (isendwin() && reset_prog_mode() == ERR) return ERR;
    wrefresh(w);
    c = count ? pushed[--count] : input(w->_delay < 0 && _cursvar.halfdelay > 0
                                      ? _cursvar.halfdelay * 100 : w->_delay);
    if (c == ERR) {
        int saved_error = errno;
        if (ripple_curses_resize()) return KEY_RESIZE;
        errno = saved_error;
        return ERR;
    }
    if (w->_keypad && c == 27) {
        s[0] = c;
        if (milliseconds(&start) == ERR) return 27;
        /* A partial escape has a bounded ambiguity delay, even in nodelay
         * mode. Empty nodelay reads never wait. Unknown bytes are preserved. */
        while (n < 32) {
            if (milliseconds(&now) == ERR) break;
            /* Scheduling or a host pause can deliver a wake after the deadline.
             * Drain bytes already available, but never extend that deadline. */
            c = count ? pushed[--count] : input(now - start >= 150 ? 0 : 150 - (int)(now - start));
            if (c == ERR) break;
            s[n++] = c;
            if (n == 2 && c != '[' && c != 'O') break;
            if (n >= 3 && c >= '@' && c <= '~') break;
        }
        key = n >= 3 ? sequence(s, n) : ERR;
        if (key != ERR) return key;
        while (n > 1) ungetch(s[--n]);
        return 27;
    }
    if (w->_keypad && (c == 127 || c == 8)) return KEY_BACKSPACE;
    if (_cursvar.echoit && c < 256) waddch(w, c);
    return c;
}
int wgetnstr(WINDOW *w, char *s, int limit) {
    int n = 0, c, saved_echo = _cursvar.echoit;
    if (!w || !s || limit < 0) { errno = EINVAL; return ERR; }
    s[0] = 0;
    _cursvar.echoit = 0;
    for (;;) {
        c = wgetch(w);
        if (c == ERR) { _cursvar.echoit = saved_echo; return ERR; }
        if (c == '\n' || c == '\r') break;
        if (c == 8 || c == 127 || c == KEY_BACKSPACE) {
            if (n) {
                s[--n] = 0;
                if (saved_echo && w->_curx > 0) {
                    wmove(w, w->_cury, w->_curx - 1); waddch(w, ' ');
                    wmove(w, w->_cury, w->_curx - 1);
                }
            }
        } else if (c >= 32 && c < 127 && n < limit) {
            s[n++] = c; s[n] = 0;
            if (saved_echo) waddch(w, c);
        }
    }
    _cursvar.echoit = saved_echo;
    return OK;
}
int wgetstr(WINDOW *w, char *s) {
    /* This legacy signature cannot know the caller's capacity. */
    (void)w; (void)s; errno = EINVAL; return ERR;
}
