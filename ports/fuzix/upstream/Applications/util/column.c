/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1989, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/ioctl.h>

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <ctype.h>

#define	TAB	8

static void	c_columnate(void);
static void	input(FILE *);
static void	maketbl(void);
static void	print(void);
static void	r_columnate(void);
static void	usage(void);
static int	width(const char *);

static int	termwidth = 80;		/* default terminal width */
static int	tblcols;		/* number of table columns for -t */

static int	entries;		/* number of records */
static int	eval;			/* exit value */
static int	maxlength;		/* longest record */
static char	**list;			/* array of pointers to records */
static const char *separator = "\t "; /* field separator for table option */

static int limit(const char *s, int max) {
    char *end;
    long n;
    errno = 0; n = strtol(s, &end, 10);
    if (errno || end == s || *end || n < 1 || n > max) errx(1, "invalid width %s", s);
    return (int)n;
}

static int tabify(int n)
{
	n += 7;		/* Tab spacing */
	n &= ~7;	/* Align to next tab */
	return n;
}

int
main(int argc, char **argv)
{
	struct winsize win;
	FILE *fp;
	int ch, tflag, xflag;
	char *p;
	const char *src;
	char *newsep;
	size_t seplen;

	if (ioctl(1, TIOCGWINSZ, &win) == -1 || !win.ws_col) {
		if ((p = getenv("COLUMNS")))
			termwidth = atoi(p);
	} else
		termwidth = win.ws_col;

	tflag = xflag = 0;
	while ((ch = getopt(argc, argv, "c:l:s:tx")) != -1)
		switch(ch) {
		case 'c':
			errno = 0;
			termwidth = limit(optarg, 1000);
			if (termwidth < 1 || termwidth > 1000)
				errx(1, "invalid terminal width \"%s\"", optarg);
			break;
		case 'l':
			tblcols = limit(optarg, 256);
			if (tblcols < 1 || tblcols > 256)
				errx(1, "invalid max width \"%s\"",
				    optarg);
			break;
		case 's':
		        newsep = strdup(optarg);
			if (newsep == NULL)
				err(1, NULL);
			separator = newsep;
			break;
		case 't':
			tflag = 1;
			break;
		case 'x':
			xflag = 1;
			break;
		case '?':
		default:
			usage();
		}
	argc -= optind;
	argv += optind;

	if (tblcols && !tflag)
		errx(1, "the -l flag cannot be used without the -t flag");

	if (!*argv)
		input(stdin);
	else for (; *argv; ++argv)
		if ((fp = fopen(*argv, "r"))) {
			input(fp);
			fclose(fp);
		} else {
			warn("%s", *argv);
			eval = 1;
		}

	if (!entries)
		exit(eval);

	if (termwidth < 1 || termwidth > 1000) termwidth = 80;
	maxlength = tabify(maxlength + 1);
	if (tflag)
		maketbl();
	else if (maxlength >= termwidth)
		print();
	else if (xflag)
		c_columnate();
	else
		r_columnate();
	exit(eval);
}

static void
c_columnate(void)
{
	int chcnt, col, cnt, endcol, numcols;
	char **lp;

	numcols = termwidth / maxlength;
	endcol = maxlength;
	for (chcnt = col = 0, lp = list;; ++lp) {
		printf("%s", *lp);
		chcnt += width(*lp);
		if (!--entries)
			break;
		if (++col == numcols) {
			chcnt = col = 0;
			endcol = maxlength;
			putchar('\n');
		} else {
			while ((cnt = tabify(chcnt + 1)) <= endcol) {
				putchar('\t');
				chcnt = cnt;
			}
			endcol += maxlength;
		}
	}
	if (chcnt)
		putchar('\n');
}

static void
r_columnate(void)
{
	int base, chcnt, cnt, col, endcol, numcols, numrows, row;

	numcols = termwidth / maxlength;
	numrows = entries / numcols;
	if (entries % numcols)
		++numrows;

	for (row = 0; row < numrows; ++row) {
		endcol = maxlength;
		for (base = row, chcnt = col = 0; col < numcols; ++col) {
			printf("%s", list[base]);
			chcnt += width(list[base]);
			if ((base += numrows) >= entries)
				break;
			while ((cnt = tabify(chcnt + 1)) <= endcol) {
				putchar('\t');
				chcnt = cnt;
			}
			endcol += maxlength;
		}
		putchar('\n');
	}
}

static void
print(void)
{
	int cnt;
	char **lp;

	for (cnt = entries, lp = list; cnt--; ++lp)
		printf("%s\n", *lp);
}

typedef struct _tbl {
	char **list;
	int cols, *len;
} TBL;
#define	DEFCOLS	25

static void
maketbl(void)
{
	TBL *t;
	int coloff, cnt;
	char *p, **lp;
	int *lens, maxcols;
	TBL *tbl;
	char **cols;
	char *s;

	if ((t = tbl = calloc(entries, sizeof(TBL))) == NULL)
		err(1, NULL);
	if ((cols = calloc((maxcols = DEFCOLS), sizeof(*cols))) == NULL)
		err(1, NULL);
	if ((lens = calloc(maxcols, sizeof(int))) == NULL)
		err(1, NULL);
	for (cnt = 0, lp = list; cnt < entries; ++cnt, ++lp, ++t) {
		for (p = *lp; *p && strchr(separator, *p); ++p)
			/* nothing */ ;
		for (coloff = 0; *p;) {
			cols[coloff] = p;

			if (++coloff == maxcols) {
				if (!(cols = realloc(cols, ((u_int)maxcols +
				    DEFCOLS) * sizeof(char *))) ||
				    !(lens = realloc(lens,
				    ((u_int)maxcols + DEFCOLS) * sizeof(int))))
					err(1, NULL);
				memset((char *)lens + maxcols * sizeof(int),
				    0, DEFCOLS * sizeof(int));
				maxcols += DEFCOLS;
			}

			if ((!tblcols || coloff < tblcols) &&
			    (s = strpbrk(p, separator))) {
				*s++ = '\0';
				while (*s && strchr(separator, *s))
					++s;
				p = s;
			} else
				break;
		}
		if (!coloff) continue;
		if ((t->list = calloc(coloff, sizeof(*t->list))) == NULL)
			err(1, NULL);
		if ((t->len = calloc(coloff, sizeof(int))) == NULL)
			err(1, NULL);
		for (t->cols = coloff; --coloff >= 0;) {
			t->list[coloff] = cols[coloff];
			t->len[coloff] = width(cols[coloff]);
			if (t->len[coloff] > lens[coloff])
				lens[coloff] = t->len[coloff];
		}
	}
	for (cnt = 0, t = tbl; cnt < entries; ++cnt, ++t) {
		if (!t->cols) { putchar('\n'); continue; }
		for (coloff = 0; coloff < t->cols  - 1; ++coloff)
			printf("%s%*s", t->list[coloff],
			    lens[coloff] - t->len[coloff] + 2, " ");
		printf("%s\n", t->list[coloff]);
		free(t->list);
		free(t->len);
	}
	free(lens);
	free(cols);
	free(tbl);
}

#define	DEFNUM		64
#define LINE_MAX	256
#define	MAXLINELEN	(LINE_MAX + 1)

static void
input(FILE *fp)
{
	static int maxentry;
	int len;
	char *p, buf[MAXLINELEN];

	if (!list)
		if ((list = calloc((maxentry = DEFNUM), sizeof(*list))) ==
		    NULL)
			err(1, NULL);
	while (fgets(buf, MAXLINELEN, fp)) {
		for (p = buf; *p && isspace(*p); ++p);
		if (!*p)
			continue;
		if (!(p = strchr(p, '\n'))) {
			if (!feof(fp)) {
                int ch;
                while ((ch = getc(fp)) != EOF && ch != '\n');
                warnx("line too long"); eval = 1; continue;
            }
            p = buf + strlen(buf);
		}
		*p = '\0';
		len = width(buf);
		if (maxlength < len)
			maxlength = len;
		if (entries == maxentry) {
			maxentry += DEFNUM;
			if (!(list = realloc(list,
			    (u_int)maxentry * sizeof(*list))))
				err(1, NULL);
		}
		list[entries] = strdup(buf);
		if (list[entries] == NULL)
			err(1, NULL);
		entries++;
	}
    if (ferror(fp)) { warn("read"); eval = 1; }
}

/* We don't support wide chars, half chars and all the other unicode fun */
static int width(const char *c)
{
    return strlen(c);
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: column [-tx] [-c columns] [-l tblcols]"
	    " [-s sep] [file ...]\n");
	exit(1);
}
