/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bounded, select-driven byte and Unicode input for the shared C16 curses port. */
#include <curses.h>
#include <errno.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include "curspriv.h"

static int pushed[64], count, batching, paste_active, paste_match;
#ifdef RIPPLE_CURSES_HOST
static unsigned char host_input[4096];
static unsigned host_at, host_used;
static int host_closed;
int ripple_curses_input_closed(void) { return host_closed; }
#endif
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
    int fd = fileno(stdin), n;
#ifdef RIPPLE_CURSES_HOST
    if (host_at < host_used) return host_input[host_at++];
    if (host_closed) return ERR;
#else
    if (stdin->pushed != EOF) { n = stdin->pushed; stdin->pushed = EOF; return n; }
    if (stdin->pos < stdin->len) return (unsigned char)stdin->buf[stdin->pos++];
#endif
    if (fd < 0 || fd >= FD_SETSIZE) { errno = EBADF; return ERR; }
    FD_ZERO(&readset); FD_SET(fd, &readset);
    if (delay >= 0) {
        timeout.tv_sec = (unsigned)delay / 1000;
        timeout.tv_usec = ((unsigned long)delay % 1000) * 1000;
        wait = &timeout;
    }
    n = select(fd + 1, &readset, NULL, NULL, wait);
    if (n <= 0) return ERR;
#ifdef RIPPLE_CURSES_HOST
    n = read(fd, host_input, batching ? sizeof(host_input) : 1);
    if (n <= 0) { if (!n || errno == EIO) host_closed = 1; return ERR; }
    host_at = 1; host_used = n;
    return host_input[0];
#else
    n = read(fd, stdin->buf, batching ? sizeof(stdin->buf) : 1);
    if (n <= 0) { if (!n) stdin->eof = 1; return ERR; }
    stdin->pos = 1; stdin->len = n;
    return (unsigned char)stdin->buf[0];
#endif
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
    case 200: paste_active = 1; paste_match = 0; return KEY_PASTE_BEGIN;
    case 201: return KEY_PASTE_END;
    default: return ERR;
    }
}
int wgetch(WINDOW *w) {
    int c, s[32], n = 1, key;
    unsigned long start, now;
    if (!w || !stdscr) { errno = EINVAL; return ERR; }
    if (!batching && ripple_curses_resize()) return KEY_RESIZE;
    if (isendwin() && reset_prog_mode() == ERR) return ERR;
    if (!batching) wrefresh(w);
again:
    c = count ? pushed[--count] : input(w->_delay < 0 && _cursvar.halfdelay > 0
                                      ? _cursvar.halfdelay * 100 : w->_delay);
    if (c == ERR) {
        int saved_error = errno;
        if (ripple_curses_resize()) return KEY_RESIZE;
        errno = saved_error;
        return ERR;
    }
    if (paste_active) {
        static const char ending[] = "\033[201~";
        if (c == ending[paste_match]) {
            if (++paste_match == 6) { paste_active = paste_match = 0; return KEY_PASTE_END; }
            goto again;
        }
        if (paste_match) {
            ungetch(c);
            while (paste_match > 1) ungetch(ending[--paste_match]);
            paste_match = 0; return 27;
        }
        return c;
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
/* The status distinguishes Unicode text from legacy KEY_* codes. A partial
 * sequence survives a polling timeout; invalid continuations are retried. */
int wget_wch(WINDOW *w, uint32_t *value) {
    int c, decoded, echo;
    if (!w || !value) { errno = EINVAL; return ERR; }
    echo = _cursvar.echoit; _cursvar.echoit = 0;
    for (;;) {
        c = wgetch(w);
        if (c == ERR) {
            if ((feof(stdin)
#ifdef RIPPLE_CURSES_HOST
                 || host_closed
#endif
                ) && w->_input_utf8.need) {
                w->_input_utf8.need = 0; *value = RU_REPLACEMENT; break;
            }
            _cursvar.echoit = echo; return ERR;
        }
        if (c > 255) {
            if (w->_input_utf8.need) { ungetch(c); w->_input_utf8.need = 0; *value = RU_REPLACEMENT; break; }
            *value = c; _cursvar.echoit = echo; return KEY_CODE_YES;
        }
        decoded = ru_decode(&w->_input_utf8, c, value);
        if (decoded < 0) ungetch(c);
        if (decoded) break;
    }
    _cursvar.echoit = echo;
    if (echo) waddch(w, *value);
    return OK;
}
/* Read complete Unicode scalars already available, without screen refreshes or
 * per-octet syscalls. Keep function/control events for the ordinary input path. */
unsigned ripple_curses_text_run(WINDOW *w, unsigned char *out, unsigned limit, int literal) {
    unsigned n = 0, size; unsigned char bytes[4]; uint32_t cp; int status, delay;
    if (!w || !out) return 0;
    delay = w->_delay; wtimeout(w, 0); batching = 1;
    while (limit - n >= 4) {
        status = wget_wch(w, &cp);
        if (status == ERR) break;
        if (status == KEY_CODE_YES || (!literal && (cp < 32 || cp == 127))) { ungetch((int)cp); break; }
        size = ru_encode(cp, bytes); memcpy(out + n, bytes, size); n += size;
    }
    batching = 0; wtimeout(w, delay); return n;
}
int wgetnstr(WINDOW *w, char *s, int limit) {
    int n=0,status,saved_echo=_cursvar.echoit,startx,starty;uint32_t cp;
    unsigned char bytes[4];unsigned size;size_t previous;unsigned oldwidth;
    if(!w||!s||limit<0){errno=EINVAL;return ERR;}
    s[0]=0;startx=w->_curx;starty=w->_cury;_cursvar.echoit=0;
    for(;;){
        status=wget_wch(w,&cp);
        if(status==ERR){_cursvar.echoit=saved_echo;return ERR;}
        if(status==OK&&(cp=='\n'||cp=='\r'))break;
        if((status==KEY_CODE_YES&&cp==KEY_BACKSPACE)||(status==OK&&(cp==8||cp==127))){
            if(n){
                previous=ru_previous_grapheme((const unsigned char *)s,n,n);
                oldwidth=ru_columns((const unsigned char *)s+previous,n-previous);
                n=previous;s[n]=0;
                if(saved_echo&&w->_cury==starty){
                    int x=startx+ru_columns((const unsigned char *)s,n);unsigned i;
                    wmove(w,starty,x);for(i=0;i<oldwidth;i++)ripple_curses_scalar(w,' ');wmove(w,starty,x);
                }
            }
        }else if(status==OK&&cp>=32&&ru_width(cp)>=0){
            size=ru_encode(cp,bytes);
            if(size<=(unsigned)(limit-n)){
                memcpy(s+n,bytes,size);n+=size;s[n]=0;if(saved_echo)waddch(w,cp);
            }
        }
    }
    _cursvar.echoit=saved_echo;return OK;
}
int wgetstr(WINDOW *w, char *s) {
    /* This legacy signature cannot know the caller's capacity. */
    (void)w; (void)s; errno = EINVAL; return ERR;
}
