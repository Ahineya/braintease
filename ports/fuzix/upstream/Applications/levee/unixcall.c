/*
 * LEVEE, or Captain Video;  A vi clone
 *
 * Copyright (c) 1982-1997 David L Parsons
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by David L Parsons (orc@pell.chi.il.us).  My name may not be used
 * to endorse or promote products derived from this software without
 * specific prior written permission.  THIS SOFTWARE IS PROVIDED
 * AS IS'' AND WITHOUT ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING,
 * WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF MERCHANTIBILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE.
 */
/*
 * Unix interface for levee
 */
#include "levee.h"
#include "extern.h"
#include <termios.h>
#ifdef GCC
#include <ioctls.h>
#endif
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>

int
min(int a, int b)
{
    return (a>b) ? b : a;
}

int
max(int a,int b)
{
    return (a<b) ? b : a;
}

int strput(char *s)
{
    if (s)
	write(1, s, strlen(s));
}


#ifndef GCC
char *basename(char *s)
{
    register char *p;

    if (p=strrchr(s,'/'))
	return 1+p;
    return s;
}
#endif


static int ioset = 0;
static struct termios old;
static void suspend_editor(int sig) {
    fixcon();
    /* signal() is one-shot: this second TSTP uses the default stop action. */
    kill(getpid(), SIGTSTP);
    signal(SIGTSTP, suspend_editor);
    initcon();
    redisplay(TRUE);
}

void initcon(void)
{
    struct termios new;

    if (!ioset) {
        signal(SIGTSTP, suspend_editor);
	tcgetattr(0, &old);	/* preserve current terminal setup */

        erasechar = old.c_cc[VERASE];
        eraseline = old.c_cc[VKILL];

        memcpy(&new, &old, sizeof(new));

	new.c_iflag &= ~(IXON|IXOFF|IXANY|ICRNL|INLCR);
	new.c_lflag &= ~(ICANON|ISIG|ECHO);
	new.c_oflag = 0;
        new.c_cc[VMIN] = 1;
        new.c_cc[VTIME] = 0;

        strput("\033[?1049h\033[H");
	tcsetattr(0, TCSANOW, &new);	/* terminal setup for editor */
        ioset=1;
    }
}

void fixcon(void)
{
    if (ioset) {
         strput("\033[?1049l");
	 tcsetattr(0, TCSANOW, &old);	/* restore original terminal setup */
         ioset = 0;
    }
}

int getKey(void)
{
    unsigned char c;

    for (;;) {
        if (read(0, &c, 1) != 1) {
            if (errno == EINTR) continue;
            fixcon(); exit(1);
        }
        if (c == 26) { raise(SIGTSTP); continue; }
        return c < 128 ? c : 0;
    }
}
