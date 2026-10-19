/* UNIX V7 source code: see /COPYRIGHT or www.tuhs.org for details. */

/*
 *	UNIX shell
 *
 *	S. R. Bourne
 *	Bell Telephone Laboratories
 *
 */

#define TRUE	(-1)
#define FALSE	0
#define LOBYTE	0377
/* C16 has a spare ninth bit; keep UTF-8 octets separate from quoting. */
#ifdef RIPPLE_MMU
#define STRIP 0377
#define QUOTE 0400
#else
#define STRIP 0177
#define QUOTE 0200
#endif

#define EOF	0
#define NL	'\n'
#define SP	' '
#define LQ	'`'
#define RQ	'\''
#define MINUS	'-'
#define COLON	':'

#define MAX(a,b)	((a)>(b)?(a):(b))
