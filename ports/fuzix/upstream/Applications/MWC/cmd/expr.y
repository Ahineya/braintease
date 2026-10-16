/*
 * cmd/expr.y
 * yacc grammar for expr.
 * Designed so no stdio is called,
 * which makes the final object code about 1/3 smaller.
 * To make, say
 *	yacc expr.y; cc -O -o expr y.tab.c;
 */

%{
extern void *ripple_absolute_pointer(void *);

#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

#define	LONGLEN	12		/* max length of NUL-terminated ASCII long */
#define	CODELEN	512		/* initial length of code buffer */
#define	CODEINC	128		/* code buffer length increment */

#define	EOF	(-1)
#define TRUE	(0 == 0)
#define FALSE	(0 != 0)
#define true(e)	(*(e) == '\0' || (isnum(e) && atol(e) == 0) ? FALSE : TRUE)

int yyparse(void);
int	exstat;		/* exit status returned by main() */
char	*result;	/* final expr printed by main */
char	s0[] = "0";	/* `0' integer - `false' expr */
char	s1[] = "1";	/* `1' integer - `true' expr */

long expr_number(char *text);
char *ltoascii(long n);
char *regexp(char *e1, char *e2);
char *arithop(int op, char *e1, char *e2);
char *relop(int op, char *e1, char *e2);
int isnum(char *e);
void compile(char *e);
char *match(char *lp, char *cp);
char *overflow(char *pc);
void output(char *s, int fildes);
int yylex(void);
void yyerror(const char *p);
void expr_regerror(void);
void dieat (void);
void divzero(void);
void errexit(void);

/*
 * The following names are used by the regular expression operator.
 */
#define BRSIZE	10			/* Length of brace list */
typedef	struct {
	char	*b_bp;			/* Ptr to start of string matched */
	char	*b_ep;			/* Ptr to end of string matched */
} BRACE;

#define CSNUL	000			/* End of expression */
#define CSSOL	001			/* Match start of line */
#define CSEOL	002			/* End of line */
#define CSOPR	003			/* \( */
#define CSCPR	004			/* \) */
#define CSBRN	005			/* Match nth brace */
#define CSDOT	006			/* Any character */
#define CMDOT	007			/* Stream of any characters */
#define CSCHR	010			/* Match given character */
#define CMCHR	011			/* Match stream of given characters */
#define CSCCL	014			/* Character class */
#define CMCCL	015			/* Stream of character class */
#define CSNCL	016			/* Not character class */
#define CMNCL	017			/* Stream of not char class */

#define	getx(c)		*e++
#define ungetx(c)	--e

BRACE	brlist[BRSIZE];			/* brace list */
int	brcount;			/* # of braces in reg_expr */
char	*codebuf;			/* Ptr to a compiled regular expr */
int	cbsiz = CODELEN;			/* Initial size of codebuf */

#define YYSTYPE	char *

char	**av;		/* Global version of argv[] in main() */
int	avx;		/* Index into av[] */
%}




%token	STR

%left	'|'
%left	'&'
%left	'<' '>' LE GE EQ NEQ	/* relop tokens */
%left	'+' '-'			/* arithop tokens */
%left	'*' '/' '%'		/* arithop tokens */
%left	':'			/* regexp token */
%right	UMINUS '!' LEN		/* uop tokens */




%%




start:
	expr		{ result = $1; exstat = true(result) ? 0 : 1; }
;

expr:
	'(' expr ')'	{ $$ = $2; }

|	LEN expr	{ $$ = ltoascii((long)strlen($2)); }
|	'!' expr	{ $$ = true($2) ? s0 : s1; }
|	'-' expr %prec UMINUS
			{ if (isnum($2))
				{ long value = expr_number($2);
				  if (value == LONG_MIN) yyerror("integer overflow");
				  $$ = ltoascii(-value); }
			else {
				--avx;
				yyerror("");
			}
			}

|	expr ':' expr	{ $$ = regexp($1, $3); }

|	expr '*' expr	{ $$ = arithop('*', $1, $3); }
|	expr '/' expr	{ $$ = arithop('/', $1, $3); }
|	expr '%' expr	{ $$ = arithop('%', $1, $3); }
|	expr '+' expr	{ $$ = arithop('+', $1, $3); }
|	expr '-' expr	{ $$ = arithop('-', $1, $3); }

|	expr '<' expr	{ $$ = relop('<', $1, $3); }
|	expr '>' expr	{ $$ = relop('>', $1, $3); }
|	expr LE expr	{ $$ = relop(LE, $1, $3); }
|	expr GE expr	{ $$ = relop(GE, $1, $3); }
|	expr EQ expr	{ $$ = relop(EQ, $1, $3); }
|	expr NEQ expr	{ $$ = relop(NEQ, $1, $3); }

|	expr '&' expr	{ $$ = true($3) ? $1 : s0; }
|	expr '|' expr	{ $$ = true($1) ? $1 : $3; }

|	'{' expr ',' expr '}'		{ $$ = true($2) ? $4 : s0; }
|	'{' expr ',' expr ',' expr '}'	{ $$ = true($2) ? $4 : $6; }

|	STR		{ $$ = $1; }
|	error		{ yyerror(""); }
;




%%




int main(int argc, char *argv[])
{
	if (argc == 1)
		return 2;
	av = argv;
	yyparse();
	output(result, 1);
	output("\n", 1);
	return exstat;
}


long expr_number(char *text)
{
    long value;
    errno = 0;
    value = strtol(text, NULL, 10);
    if (errno) yyerror("integer overflow");
    return value;
}

char *arithop(int op, char *e1, char *e2)
{
	register long v1, v2;

	if (!isnum(e1)) {
		avx -= 3;
		yyerror("");
	}
	if (!isnum(e2)) {
		--avx;
		yyerror("");
	}

	v1 = expr_number(e1);
	v2 = expr_number(e2);
	switch (op) {
	case '+':
		if ((v2 > 0 && v1 > LONG_MAX - v2) || (v2 < 0 && v1 < LONG_MIN - v2)) yyerror("integer overflow");
		v1 += v2;
		break;
	case '-':
		if ((v2 < 0 && v1 > LONG_MAX + v2) || (v2 > 0 && v1 < LONG_MIN + v2)) yyerror("integer overflow");
		v1 -= v2;
		break;
	case '*':
		if (v1 && v2 && ((v1 > 0 && v2 > 0 && v1 > LONG_MAX / v2) || (v1 > 0 && v2 < 0 && v2 < LONG_MIN / v1) || (v1 < 0 && v2 > 0 && v1 < LONG_MIN / v2) || (v1 < 0 && v2 < 0 && v1 < LONG_MAX / v2))) yyerror("integer overflow");
		v1 *= v2;
		break;
	case '/':
		if (v1 == LONG_MIN && v2 == -1) yyerror("integer overflow");
		if (v2 == 0) {
			divzero ();
			v1 = LONG_MAX;
		} else
			v1 /= v2;
		break;
	case '%':
		if (v1 == LONG_MIN && v2 == -1) return ltoascii(0);
		if (v2 == 0) {
			divzero ();
			v1 = 0;
		} else
			v1 %= v2;
		break;
	}
	return ltoascii(v1);
}

char *relop(int op, char *e1, char *e2)
{
	register int cmp;
	register long v1, v2;

	if (!isnum(e1) || !isnum(e2))
		cmp = strcmp(e1, e2);
	else {
		v1 = expr_number(e1);
		v2 = expr_number(e2);
		cmp = (v1 > v2) ? 1 : (v1 == v2) ? 0 : -1;
	}
	switch (op) {
	case '<':
		return (cmp < 0) ? s1 : s0;
	case '>':
		return (cmp > 0) ? s1 : s0;
	case LE:
		return (cmp <= 0) ? s1 : s0;
	case GE:
		return (cmp >= 0) ? s1 : s0;
	case EQ:
		return (cmp == 0) ? s1 : s0;
	case NEQ:
		return (cmp != 0) ? s1 : s0;
	}
}


char *regexp(char *e1, char *e2)
{
	register char *a = e1;
	register char *b;
	register BRACE *brp;

	cbsiz = CODELEN;
	codebuf = malloc(cbsiz);
	if (!codebuf) expr_regerror();
	memset(brlist, 0, sizeof(brlist));
	compile(e2);
	if (brcount > 0)	/* brcount is now the number of braces in e2 */
		brlist[brcount].b_bp = brlist[brcount].b_ep = NULL;
#if	1
	/* Posix P1003.2 4.22.7.1: pattern search is anchored to beginning. */
	b = match(a, codebuf + (codebuf[0] == CSSOL));
#else
	/* This code searches for unanchored match. */
	if (codebuf[0] == CSSOL)
		b = match(a, codebuf + 1);
	else
		for ( ; *a != '\0'; ++a)
			if ((b = match(a, codebuf)) != NULL)
				break;
#endif
    if (b == NULL) { free(codebuf); return brcount ? "" : "0"; }
    if (!brcount) {
        long matched = b - e1;
        free(codebuf);
        return ltoascii(matched);
    }
    if (!brlist[0].b_bp || !brlist[0].b_ep) { free(codebuf); return ""; }
    {
        size_t n = brlist[0].b_ep - brlist[0].b_bp;
        char *text = malloc(n + 1);
        if (!text) { output("expr: out of memory\n", 2); exit(3); }
        memcpy(text, brlist[0].b_bp, n); text[n] = 0;
        free(codebuf);
        return text;
    }

}


int isnum(char *e)
{
	register int c;

	if ((c = * e ++) == '-')	/* || c == '+' Removed for POSIX */
		c = * e ++;
	do
		if (! isdigit (c))
			return FALSE;
	while ((c = * e ++) != 0);
	return TRUE;
}

/*
 * Convert long to ascii. Return pointer to the necessary malloced storage.
 */
char *ltoascii(long n)
{
	char buf[LONGLEN];
	unsigned long magnitude = n < 0 ? 0UL - (unsigned long)n : (unsigned long)n;
	register char *bp = buf;
	register char *ep;
	register char *e;

	e = ep = malloc(LONGLEN);
	if (n < 0) {
		*ep++ = '-';
	}
	do {
		*bp++ = (magnitude % 10) + '0';
		magnitude /= 10;
	} while (magnitude > 0);
	while (bp > buf)
		*ep++ = *--bp;
	*ep = '\0';
	return e;
}


/*
 * Compile the regular expression e into codebuf.
 * Invoke expr_regerror() on a regular expression syntax error.
 */
void compile(char *e)
{
	register int c;
	register char *cp, *lcp;
	int blevel, n, notflag, bstack[BRSIZE + 1];

	brcount = 0;
	blevel = 0;
	cp = &codebuf[0];
	if ((c = getx(c)) == '^') {
		*cp++ = CSSOL;
		c = getx(c);
	}
	while (c != '\0') {
		if (cp > &codebuf[cbsiz-4])
			cp = overflow(cp);
		switch (c) {
		case '*':
			expr_regerror();
		case '.':
			if ((c = getx(c)) != '*') {
				*cp++ = CSDOT;
				continue;
			}
			*cp++ = CMDOT;
			c = getx(c);
			continue;
		case '$':
			if ((c = getx(c)) != '\0') {
				ungetx(c);
				c = '$';
				goto character;
			}
			*cp++ = CSEOL;
			continue;
		case '[':
			/*
			 * lcp[0] will contain C<S|M><C|N>CL. lcp[1] will be
			 * the number of chars in the class. These are followed
			 * by the members of the class singly enumerated.
			 * ']' is valid only at the start of the member list.
			 * '-' is valid only at the end of the member list.
			 */
			lcp = cp;
			if ((c = getx(c)) == '^')
				notflag = TRUE;
			else {
				notflag = FALSE;
				ungetx(c);
			}
			cp += 2;
			if ((c = getx(c)) == ']')
				*cp++ = c;
			else
				ungetx(c);
			while ((c = getx(c)) != ']') {
				if (c == '\0')
					expr_regerror();
				if (c!='-' || cp==lcp+2) {
					if (cp >= &codebuf[cbsiz-4])
						cp = overflow(cp);
					*cp++ = c;
					continue;
				}

				/* c = '-' now. Lookahead at the next char */
				if ((c = getx(c)) == '\0')
					expr_regerror();
				if (c == ']') {
					*cp++ = '-';
					ungetx(c);
					continue;
				}
				if ((n=cp[-1]) > c)
					expr_regerror();
				while (++n <= c) {
					if (cp >= &codebuf[cbsiz-4])
						cp = overflow(cp);
					*cp++ = n;
				}
			}
			if ((c = getx(c)) == '*') {
				lcp[0] = (notflag) ? CMNCL : CMCCL;
				c = getx(c);
			}
			else
				lcp[0] = (notflag) ? CSNCL : CSCCL;
			if ((n=cp-(lcp+2)) > 255)
				expr_regerror();
			*++lcp = n;
			continue;
		case '\\':
			switch (c = getx(c)) {
			case '\0':
				expr_regerror();
			case '(':
				if (blevel >= BRSIZE - 1 || brcount >= BRSIZE - 1) expr_regerror();
				*cp++ = CSOPR;
				*cp++ = bstack[blevel++] = brcount++;
				c = getx(c);
				continue;
			case ')':
				if (blevel == 0)
					expr_regerror();
				*cp++ = CSCPR;
				*cp++ = bstack[--blevel];
				c = getx(c);
				continue;
			default:
				if (isascii(c) && isdigit(c)) {
					*cp++ = CSBRN;
					if (c == '0' || c - '0' > brcount) expr_regerror();
					*cp++ = c-'0' - 1;
					c = getx(c);
					continue;
				}
			}
		default:
		character:
			*cp++ = CSCHR;
			*cp++ = c;
			if ((c = getx(c)) == '*') {
				cp[-2] = CMCHR;
				c = getx(c);
			}
		}
	}
	if (blevel) expr_regerror();
	*cp++ = CSNUL;
	return;
}

/*
 * Given a pointer to a compiled expression `cp' and a pointer to a line `lp',
 * return a ptr to the char following the last char of the match
 * if successful, NULL otherwise.
 */
char *match(char *lp, char *cp)
{
	register int n;
	char *llp, *lcp;

	for (;;) {
		switch (*cp++) {
		case CSNUL:
			return lp;
		case CSEOL:
			if (*lp)
				return NULL;
			return lp;
		case CSOPR:
			brlist[*cp++].b_bp = lp;
			continue;
		case CSCPR:
			brlist[*cp++].b_ep = lp;
			continue;
		case CSBRN:
			n = *cp++;
			if (!brlist[n].b_bp || !brlist[n].b_ep) expr_regerror();
			lcp = cp;
			cp = brlist[n].b_bp;
			n = brlist[n].b_ep - cp;
			if (n > strlen(lp))
				return NULL;
			while (n-- > 0)
				if (*lp++ != *cp++)
					return NULL;
			cp = lcp;
			continue;
		case CSDOT:
			if (*lp++ == '\0')
				return NULL;
			continue;
		case CMDOT:
			llp = lp;
			while (*lp)
				lp++;
			goto star;
		case CSCHR:
			if (*cp++ != *lp++)
				return NULL;
			continue;
		case CMCHR:
			llp = lp;
			while (*cp == *lp)
				lp++;
			cp++;
			goto star;
		case CSCCL:
			n = *cp++;
			while (*cp++ != *lp)
				if (--n == 0)
					return NULL;
			lp++;
			cp += n-1;
			continue;
		case CMCCL:
			llp = lp;
			lcp = cp;
			while (*lp) {
				cp = lcp;
				n = *cp++;
				while (*cp++ != *lp)
					if (--n == 0)
						goto star;
				lp++;
			}
			cp = lcp + *lcp + 1;
			goto star;
		case CSNCL:
			if (*lp == '\0')
				return NULL;
			n = *cp++;
			while (n--)
				if (*cp++ == *lp)
					return NULL;
			lp++;
			continue;
		case CMNCL:
			llp = lp;
			lcp = cp;
			while (*lp) {
				cp = lcp;
				n = *cp++;
				while (n--) {
					if (*cp++ == *lp) {
						cp = lcp + *lcp + 1;
						goto star;
					}
				}
				lp++;
			}
			cp = lcp + *lcp + 1;
		star:
			do {
				if ((lcp = match(lp, cp)) != 0)
					return lcp;
			} while (--lp >= llp);
			return NULL;
		}
	}
}

/*
 * overflow enlarges codebuf by CODEINC bytes. The argument is a pointer
 * to a position in codebuf - the function returns a pointer with the same
 * relative position in the new buffer.
 */
char *overflow(char *pc)
{
	register int posn = pc - codebuf;
	if (cbsiz > 16000) expr_regerror();

	if ((codebuf = realloc(codebuf, cbsiz += CODEINC)) == NULL)
		expr_regerror();
	return codebuf + posn;
}

/*
 * An output function to avoid having to include stdio.
 */
void output(char *s, int fildes)
{
	register int len;

	len = strlen(s);
	if ((write(fildes, s, len)) != len)
		exit(3);
}


int yylex(void)
{
	register int c;

	if ((yylval = av[++avx]) == NULL)
		return EOF;
	if (av[avx][1] == '\0')
		switch (c = av[avx][0]) {
		case '{':
		case '}':
		case ',':
		case '|':
		case '&':
		case '=':
			return EQ;
		case '<':
		case '>':
		case '+':
		case '-':
		case '*':
		case '/':
		case '%':
		case ':':
		case '!':
		case '(':
		case ')':
			return c;
		default:
			return STR;
		}
	if (av[avx][1] == '='  &&  av[avx][2] == '\0')
		switch (c = av[avx][0]) {
		case '<':
			return LE;
		case '>':
			return GE;
		case '=':
			return EQ;
		case '!':
			return NEQ;
		default:
			return STR;
		}
	return (strcmp(yylval, "len") == 0) ? LEN : STR;
}

/*
 * The common code between yyerror() and expr_regerror() (in regexp.c) is split
 * off into errexit().
 */
void yyerror(const char *p)
{
	output("expr: ", 2);
	errexit();
}

void expr_regerror(void)
{
	output("expr: regular expression ", 2);
	--avx;
	errexit();
}

void dieat (void)
{
	output(ltoascii((long) avx), 2);
	output("\n", 2);
	exit(2);
}

void divzero(void)
{
	output ("Attempt to divide by zero at argument # ", 2);
	-- avx;
	dieat ();
}

void errexit(void)
{
	output("syntax error at argument # ", 2);
	dieat ();
}

/* end of cmd/expr.y */
