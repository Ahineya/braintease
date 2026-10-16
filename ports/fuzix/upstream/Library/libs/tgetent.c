/*
 *	termcap.c	V1.1	20/7/87		agc	Joypace Ltd
 *
 *	Copyright Joypace Ltd, London, UK, 1987. All rights reserved.
 *	This file may be freely distributed provided that this notice
 *	remains attached.
 *
 *	A public domain implementation of the termcap(3) routines.
 *
 *
 *
 *	 Klamer Schutte	      V1.2    Nov. 1988
 *
 *   - Can match multiple terminal names		 [tgetent]
 *   - Removal of **area assignments			 [tgetstr]
 *
 *	 Terrence W. Holm     V1.3    May, Sep, Oct.  1988
 *
 *  - Correct when TERM != name and TERMCAP is defined	 [tgetent]
 *  - Correct the comparison for the terminal name 	 [tgetent]
 *  - Correct the value of ^x escapes              	 [tgetstr]
 *  - Added %r to reverse row/column			 [tgoto]
 *  - Fixed end of definition test			 [tgetnum/flag/str]
 *
 *	 Terrence W. Holm     V1.4    Jan. 1989
 *
 *   - Incorporated Klamer's V1.2 fixes into V1.3
 *   - Added %d, (old %d is now %2)			 [tgoto]
 *   - Allow '#' comments in definition file		 [tgetent]
 *
 * FIXME:
 *	- support the use of some kind of < 1024 byte buffer
 *	  interface
 *	- avoid the use of stdio (state machine search for either
 *	  [start of file]name: or \nname:)
 */

#include <termcap.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

extern char *capab;		/* the capability itself */

#if 0
/*  The following are not yet used.  */
extern short ospeed;		/* output speed */
extern char PC;			/* padding character */
extern char *BC;		/* back cursor movement */
extern char *UP;		/* up cursor movement */
#endif


/*
 *	tgetent - get the termcap entry for terminal name, and put it
 *	in bp (which must be an array of 1024 chars). Returns 1 if
 *	termcap entry found, 0 if not found, and -1 if file not found.
 */

/* Ripple adaptation: bounded logical entries, empty lines and missing TERM. */
int tgetent(char *bp, const char *name)
{
    FILE *fp;
    const char *file = getenv("TERMCAP");
    char *term = getenv("TERM");
    size_t used = 0, len;
    int overflow = 0;
    capab = NULL;
    if (!bp || !name || !*name) return 0;
    len = strlen(name);
    if (file && *file != '/' && term && !strcmp(term, name)) {
        if (strlen(file) >= 1024) return 0;
        strcpy(bp, file);
        capab = bp;
        return 1;
    }
    if (!file || *file != '/') file = "/etc/termcap";
    fp = fopen(file, "r");
    if (!fp) return -1;
    for (;;) {
        int c = getc(fp);
        if (c == EOF && !used && !overflow) break;
        if (c != EOF && c != '\n') {
            if (used < 1023) bp[used++] = c;
            else overflow = 1;
            continue;
        }
        if (!overflow && used && bp[used - 1] == '\\' && c != EOF) {
            used--;
            continue;
        }
        bp[used] = 0;
        if (!overflow) {
            char *cp = bp;
            while (isspace(*cp)) cp++;
            if (*cp && *cp != '#') {
                do {
                    if (!strncmp(cp, name, len) &&
                        (cp[len] == '|' || cp[len] == ':')) {
                        fclose(fp);
                        capab = bp;
                        return 1;
                    }
                    while (*cp && *cp != '|' && *cp != ':') cp++;
                } while (*cp++ == '|');
            }
        }
        used = 0;
        overflow = 0;
        if (c == EOF) break;
    }
    fclose(fp);
    return 0;
}
