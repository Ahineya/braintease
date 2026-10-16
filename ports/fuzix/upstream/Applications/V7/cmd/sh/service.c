/* UNIX V7 source code: see /COPYRIGHT or www.tuhs.org for details. */
/* Changes: Copyright (c) 1999 Robert Nordier. All rights reserved. */

/*
 * UNIX shell
 *
 * S. R. Bourne
 * Bell Telephone Laboratories
 *
 */

#include	"defs.h"
#include <string.h>
#include <sys/wait.h>


static const char *execs(const char *ap, const char *t[]);
static void gsort(char *from[], char *to[]);
static int split(const char *s);

#define ARGMK	01

/* fault handling */
#define ENOMEM	12
#define ENOEXEC 8
#define E2BIG	7
#define ENOENT	2
#define ETXTBSY 26



/* service routines for `execute' */

void initio(IOPTR iop)
{
	register char *ion;
	register int iof, fd;

	if (iop) {
		iof = iop->iofile;
		ion = mactrim(iop->ioname);
		if (*ion && (flags & noexec) == 0) {
			if (iof & IODOC) {
				subst(chkopen(ion), (fd = tmpfil()));
				close(fd);
				fd = chkopen(tmpout);
				unlink(tmpout);
			} else if (iof & IOMOV) {
				if (eq(minus, ion)) {
					fd = -1;
					close(iof & IOUFD);
				} else if ((fd = stoi(ion)) >= USERIO) {
					failed(ion, badfile);
				} else {
					fd = dup(fd);
				}
			} else if ((iof & IOPUT) == 0) {
				fd = chkopen(ion);
			} else if (flags & rshflg) {
				failed(ion, restricted);
			} else if (iof & IOAPP && (fd = open(ion, 1)) >= 0) {
				lseek(fd, 0L, 2);
			} else {
				fd = create(ion);
			}
			if (fd >= 0) {
				sh_rename(fd, iof & IOUFD);
			};
		}
		initio(iop->ionxt);
	}
}

const char *getpath(const char *s)
{
	register const char *path;
	if (any('/', s)) {
		if (flags & rshflg) {
			failed(s, restricted);
		} else {
			return (nullstr);
		}
	} else if ((path = pathnod.namval) == 0) {
		return (defpath);
	} else {
		return (cpystak(path));
	}
}

int pathopen(const char *path, const char *name)
{
	register UFD f;

	do {
		path = catpath(path, name);
	} while ((f = open(curstak(), 0)) < 0 && path);
	return (f);
}

const char *catpath(register const char *path, const char *name)
{
	/* leaves result on top of stack */
	register const char *scanp = path;
	register char *argp = locstak();

	while (*scanp && *scanp != COLON) {
		*argp++ = *scanp++;
	}
	if (scanp != path) {
		*argp++ = '/';
	}
	if (*scanp == COLON) {
		scanp++;
	}
	path = (*scanp ? scanp : (char *)0);
	scanp = name;
	while ((*argp++ = *scanp++));
	return (path);
}

static const char *xecmsg;
static char **xecenv;

void execa(const char **at)
{
	register const char *path;
	register const char **t = at;

	if ((flags & noexec) == 0) {
		xecmsg = notfound;
		path = getpath(*t);
		namscan(exname);
		xecenv = sh_setenv();
		while ( (path = execs(path, t)) );
		failed(*t, xecmsg);
	}
}

static const char *execs(const char *ap, const char *t[])
{
	register char *p;
	register const char *prefix;

	prefix = catpath(ap, t[0]);
	trim(p = curstak());

	sigchk();
	execve(p, (char**) &t[0], (char **)xecenv);
	switch (errno) {
	case ENOEXEC:		/* could be a shell script */
		flags = 0;
		comdiv = 0;
		ioset = 0;
		clearup();	/* remove open files and for loop junk */
		if (input) {
			close(input);
		}
		close(output);
		output = 2;
		input = chkopen(p);

		/* set up new args */
		setargs(t);
		longjmp(subshell, 1);

	case ENOMEM:
		failed(p, toobig);

	case E2BIG:
		failed(p, arglist);

	case ETXTBSY:
		failed(p, txtbsy);

	default:
		xecmsg = badexec;
	case ENOENT:
		return (prefix);
	}
}

/* for processes to be waited for */
#define MAXP 20
static int pwlist[MAXP];
static int pwc;
/* Keep exit statuses when PID 1 reaps background/adopted children, or a
 * foreground wait happens to reap an unrelated background child first. */
static int deadpid[MAXP], deadstatus[MAXP], deadnext;
static void remember(int pid, int status)
{
    int i;
    for (i = 0; i < MAXP; i++)
        if (!deadpid[i])
            break;
    if (i == MAXP) {
        i = deadnext++;
        if (deadnext == MAXP)
            deadnext = 0;
    }
    deadpid[i] = pid;
    deadstatus[i] = status;
}
static int remembered(int pid, int *status)
{
    int i;
    for (i = 0; i < MAXP; i++)
        if (deadpid[i] && (pid <= 0 || deadpid[i] == pid)) {
            pid = deadpid[i];
            *status = deadstatus[i];
            deadpid[i] = 0;
            return pid;
        }
    return 0;
}
void reapchildren(void)
{
    int pid, status;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0)
        if (!jobs_status(pid, status)) remember(pid, status);
}

void postclr(void)
{
	register int *pw = pwlist;
	while (pw <= &pwlist[pwc])
		*pw++ = 0;
	pwc = 0;
    memset(deadpid, 0, sizeof(deadpid));
}

void post(int pcsid)
{
	register int *pw = pwlist;

	if (pcsid) {
		while (*pw)
			pw++;
		if (pwc >= MAXP - 1)
			pw--;
		else
			pwc++;
		*pw = pcsid;
	}
}

void await(int i)
{
	int rc = 0, wx = 0;
	int w;
	int ipwc = pwc;

	post(i);
	while (pwc) {
		register int p;
        int tracked = 0;
		register int sig;
		int w_hi;

		{
			register int *pw = pwlist;
			p = remembered(i, &w);
            if (!p)
                p = wait(&w);
			while (pw <= &pwlist[ipwc]
			    ) {
				if (*pw == p) {
					*pw = 0;
                    tracked = 1;
					pwc--;
				} else {
					pw++;
				}
			}
		}

		if (p == -1) {
            if (errno == EINTR)
                continue;
            memset(pwlist, 0, sizeof(pwlist));
            pwc = 0;
            break;
        }

        if (!tracked && i > 0)
            remember(p, w);

		w_hi = (w >> 8) & LOBYTE;

		if ( (sig = w & 0177) ) {
			const char *msg;
			if (sig == 0177) {	/* ptrace! return */
				prs("ptrace: ");
				sig = w_hi;
			}
			if (sig && (msg = strsignal(sig))) {
				if (i != p || (flags & prompt) == 0) {
					prp();
					prn(p);
					blank();
				}
				prs(msg);
				if (w & 0200)
					prs(coredump);
			}
			newline();
		}

		/* A foreground pipeline reports its final command's status,
		 * regardless of the order in which the children are reaped. */
		if (p == i || i <= 0)
			rc = (sig ? sig | SIGFLG : w_hi);
		wx |= w;
	}

	if (wx && flags & errflg)
		exitsh(rc);
	exitval = rc;
	exitset();
}

extern BOOL nosubst;	/* FIXME */

void trim(char *at)
{
	register char *p;
	register char c;
	register char q = 0;

	if ( (p = at) ) {
		while ( (c = *p) ) {
			*p++ = c & STRIP;
			q |= c;
		}
	}
	nosubst = q & QUOTE;
}

char *mactrim(char *s)
{
	register char *t = macro(s);
	trim(t);
	return (t);
}

char **scan(int argn)
{
	register ARGPTR argp = (ARGPTR) (((intptr_t)(gchain)) & ~ARGMK);
	register char **comargn, **comargm;

	comargn = (char **) getstak(BYTESPERWORD * argn + BYTESPERWORD);
	comargm = comargn += argn;
	*comargn = ENDARGS;

	while (argp) {
		*--comargn = argp->argval;
		if ( (argp = argp->argnxt) )
			trim(*comargn);
		if (argp == 0 || ((intptr_t)(argp)) & ARGMK) {
			gsort(comargn, comargm);
			comargm = comargn;
		}
		/* Lcheat(argp) &= ~ARGMK; */
		argp = (ARGPTR) (((intptr_t)(argp)) & ~ARGMK); } return
	comargn;
}

static void gsort(char *from[], char *to[])
{
	int k, m, n;
	register int i, j;

	if ((n = to - from) <= 1)
		return;

	for (j = 1; j <= n; j *= 2);

	for (m = 2 * j - 1; m /= 2;) {
		k = n - m;
		for (j = 0; j < k; j++) {
			for (i = j; i >= 0; i -= m) {
				register char **fromi;
				fromi = &from[i];
				if (cf(fromi[m], fromi[0]) > 0) {
					break;
				} else {
					char * s;
					s = fromi[m];
					fromi[m] = fromi[0];
					fromi[0] = s;
				}
			}
		}
	}
}

/* Argument list generation */

int getarg(COMPTR ac)
{
	register ARGPTR argp;
	register int count = 0;
	register COMPTR c;

	if ( (c = ac) ) {
		argp = c->comarg;
		while (argp) {
			count += split(macro(argp->argval));
			argp = argp->argnxt;
			;
		}
		;
	}
	return (count);
}

static int split(register const char *s)
{
	register char *argp;
	register int c;
	int count = 0;

	for (;;) {
		sigchk();
		argp = locstak() + BYTESPERWORD;
		while ((c = *s++, !any(c, ifsnod.namval) && c)) {
			*argp++ = c;
		}
		if (argp == staktop + BYTESPERWORD) {
			if (c) {
				continue;
			} else {
				return (count);
				;
			}
		} else if (c == 0) {
			s--;
		}
		if ( (c = expand(((ARGPTR) (argp = endstak(argp)))->argval, 0)) ) {
			count += c;
		} else {	/* assign(&fngnod, argp->argval); */
			makearg(argp);
			count++;
		}
		Lcheat(gchain) |= ARGMK;
	}
}
