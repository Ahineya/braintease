#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"
#include "cc.h"

struct macroparam {
	char *name;
	enum {
		PARAMTOK = 1<<0,  /* the parameter is used normally */
		PARAMSTR = 1<<1,  /* the parameter is used with the '#' operator */
		PARAMVAR = 1<<2,  /* the parameter is __VA_ARGS__ */
	} flags;
};

struct macroarg {
	struct token *token;
	size_t ntoken;
	/* stringized argument */
	struct token str;
};

struct macro {
	enum {
		MACROOBJ,
		MACROFUNC,
	} kind;
	char *name;
	/* whether or not this macro is ineligible for expansion */
	bool hide;
	/* parameters of function-like macro */
	struct macroparam *param;
	size_t nparam;
	/* argument tokens of macro invocation */
	struct macroarg *arg;
	/* replacement list */
	struct token *token;
	size_t ntoken;
};

struct frame {
	struct token *token;
	size_t ntoken;
	struct macro *macro;
};

enum ppflags ppflags;

static struct array ctx;
static struct array macros;
/* number of macros currently undergoing expansion */
static size_t macrodepth;

void
ppinit(void)
{
	next();
}

static void
macrodel(struct macro *m)
{
	free(m->param);
	free(m->token);
	free(m);
}

/* check if two macro definitions are equal, as in C11 6.10.3p2 */
static bool
macroequal(struct macro *m1, struct macro *m2)
{
	struct macroparam *p1, *p2;
	struct token *t1, *t2;

	if (m1->kind != m2->kind)
		return false;
	if (m1->kind == MACROFUNC) {
		if (m1->nparam != m2->nparam)
			return false;
		for (p1 = m1->param, p2 = m2->param; p1 < m1->param + m1->nparam; ++p1, ++p2) {
			if (strcmp(p1->name, p2->name) != 0 || p1->flags != p2->flags)
				return false;
		}
	}
	if (m1->ntoken != m2->ntoken)
		return false;
	for (t1 = m1->token, t2 = m2->token; t1 < m1->token + m1->ntoken; ++t1, ++t2) {
		if (t1->kind != t2->kind)
			return false;
		if (t1->lit && strcmp(t1->lit, t2->lit) != 0)
			return false;
	}
	return true;
}

/* find the index of a macro parameter with the given name */
static size_t
macroparam(struct macro *m, struct token *t)
{
	size_t i;

	if (t->kind >= TPPIDENT) {
		for (i = 0; i < m->nparam; ++i) {
			if (strcmp(m->param[i].name, tokenstr(t->kind)) == 0)
				return i;
		}
	}
	return -1;
}

/* lookup a macro by name */
static struct macro *
macroget(enum tokenkind ident)
{
	return arraygetptr(&macros, ident - TPPIDENT);
}

static void
macrodone(struct macro *m)
{
	m->hide = false;
	if (m->kind == MACROFUNC && m->nparam > 0) {
		free(m->arg[0].token);
		free(m->arg);
	}
	--macrodepth;
}

static bool
macrovarargs(struct macro *m)
{
	return m->kind == MACROFUNC && m->nparam > 0 && m->param[m->nparam - 1].flags & PARAMVAR;
}

static struct token *
framenext(struct frame *f)
{
	return f->ntoken--, f->token++;
}

/* push a new context frame */
static struct frame *
ctxpush(struct token *t, size_t n, struct macro *m, bool space)
{
	struct frame *f;

	f = arrayadd(&ctx, sizeof(*f));
	f->token = t;
	f->ntoken = n;
	f->macro = m;
	if (n > 0)
		t[0].space = space;
	return f;
}

/* get the next token from the context */
static struct token *
ctxnext(void)
{
	struct frame *f;
	struct token *t;
	struct macro *m;
	bool space;
	size_t i;

again:
	for (f = arraylast(&ctx, sizeof(*f)); ctx.len; --f, ctx.len -= sizeof(*f)) {
		if (f->ntoken)
			break;
		if (f->macro)
			macrodone(f->macro);
	}
	if (ctx.len == 0)
		return NULL;
	m = f->macro;
	if (m && m->kind == MACROFUNC) {
		/* try to expand macro parameter */
		space = f->token->space;
		switch (f->token->kind) {
		case THASH:
			framenext(f);
			t = framenext(f);
			assert(t);
			i = macroparam(m, t);
			assert(i != -1);
			f = ctxpush(&m->arg[i].str, 1, NULL, space);
			break;
		default:
			if (f->token->kind >= TPPIDENT) {
				i = macroparam(m, f->token);
				if (i == -1)
					break;
				framenext(f);
				if (m->arg[i].ntoken == 0)
					goto again;
				f = ctxpush(m->arg[i].token, m->arg[i].ntoken, NULL, space);
			}
			break;
		}
		/* XXX: token concatenation */
	}
	return framenext(f);
}

static void
define(void)
{
	struct token *t;
	enum tokenkind ident, prev;
	struct macro *m, *other;
	struct macroparam *p;
	struct array params = {0}, repl = {0};
	size_t i;

	m = xmalloc(sizeof(*m));
	ident = tok.kind;
	m->name = tokencheck(&tok, TPPIDENT, "after #define");
	m->hide = false;
	t = arrayadd(&repl, sizeof(*t));
	scan(t);
	if (t->kind == TLPAREN && !t->space) {
		m->kind = MACROFUNC;
		/* read macro parameter names */
		p = NULL;
		while (scan(&tok), tok.kind != TRPAREN) {
			if (p) {
				if (p->flags & PARAMVAR)
					tokencheck(&tok, TRPAREN, "after '...'");
				tokencheck(&tok, TCOMMA, "or ')' after macro parameter");
				scan(&tok);
			}
			p = arrayadd(&params, sizeof(*p));
			p->flags = 0;
			if (tok.kind == TELLIPSIS) {
				p->name = "__VA_ARGS__";
				p->flags |= PARAMVAR;
			} else {
				p->name = tokencheck(&tok, TPPIDENT, "of macro parameter name or '...'");
			}
		}
		scan(t);  /* first token in replacement list */
	} else {
		m->kind = MACROOBJ;
	}
	m->param = params.val;
	m->nparam = params.len / sizeof(m->param[0]);

	/* read macro body */
	i = macroparam(m, t);
	while (t->kind != TNEWLINE && t->kind != TEOF) {
		if (t->kind == THASHHASH)
			error(&t->loc, "'##' operator is not yet implemented");
		prev = t->kind;
		t = arrayadd(&repl, sizeof(*t));
		scan(t);
		if (t->kind == T__VA_ARGS__ && !macrovarargs(m))
			error(&t->loc, "__VA_ARGS__ can only be used in variadic function-like macros");
		if (m->kind != MACROFUNC)
			continue;
		if (i != -1)
			m->param[i].flags |= PARAMTOK;
		i = macroparam(m, t);
		if (prev == THASH) {
			tokencheck(t, TPPIDENT, "after '#' operator");
			if (i == -1)
				error(&t->loc, "'%s' is not a macro parameter name", t->lit);
			m->param[i].flags |= PARAMSTR;
			i = -1;
		}
	}
	m->token = repl.val;
	m->ntoken = repl.len / sizeof(*t) - 1;
	tok = *t;

	i = ident - TPPIDENT;
	other = arraysetptr(&macros, i, m);
	if (other) {
		if (!macroequal(m, other))
			error(&tok.loc, "redefinition of macro '%s'", m->name);
		macrodel(other);
	}
}

static void
undef(void)
{
	enum tokenkind ident;
	struct macro *m;

	ident = tok.kind;
	tokencheck(&tok, TPPIDENT, "after #undef");
	m = macroget(ident);
	if (m) {
		macrodel(m);
		((void **)macros.val)[ident - TPPIDENT] = NULL;
	}
	scan(&tok);
}

static void
directive(void)
{
	struct location newloc;
	enum ppflags oldflags;
	enum tokenkind kind;

	scan(&tok);
	if (tok.kind == TNEWLINE)
		return;  /* empty directive */
	oldflags = ppflags;
	ppflags |= PPNEWLINE;
	kind = tok.kind;
	switch (kind) {
	case TNUMBER:
		goto line;  /* gcc line markers */
	case TIF:
		error(&tok.loc, "#if directive is not implemented");
		break;
	case TIFDEF:
		error(&tok.loc, "#ifdef directive is not implemented");
		break;
	case TIFNDEF:
		error(&tok.loc, "#ifndef directive is not implemented");
		break;
	case TELIF:
		error(&tok.loc, "#elif directive is not implemented");
		break;
	case TENDIF:
		error(&tok.loc, "#endif directive is not implemented");
		break;
	case TINCLUDE:
		error(&tok.loc, "#include directive is not implemented");
		break;
	case TDEFINE:
		scan(&tok);
		define();
		break;
	case TUNDEF:
		scan(&tok);
		undef();
		break;
	case TLINE:
		scan(&tok);
		tokencheck(&tok, TNUMBER, "after #line");
	line:
		newloc.line = strtoull(tok.lit, NULL, 0);
		newloc.col = 1;
		scan(&tok);
		newloc.file = tok.loc.file;
		if (tok.kind == TSTRINGLIT) {
			char *quote;

			/* XXX: handle escape sequences (reuse string decoding from expr.c) */
			quote = strchr(tok.lit, '"');
			assert(quote);
			newloc.file = quote + 1;
			quote = strchr(quote + 1, '"');
			assert(quote);
			*quote = '\0';
			scan(&tok);
		}
		while (tok.kind == TNUMBER)
			scan(&tok);
		scansetloc(newloc);
		break;
	case TERROR:
	case TWARNING:
		diagloc(&tok.loc);
		fprintf(stderr, "%s directive:", kind == TERROR ? "#error" : "#warning");
		scan(&tok);
		while (tok.kind != TNEWLINE && tok.kind != TEOF) {
			tokenprint(&tok, stderr);
			scan(&tok);
		}
		fputc('\n', stderr);
		if (kind == TERROR)
			exit(1);
		break;
	case TPRAGMA:
		while (tok.kind != TNEWLINE && tok.kind != TEOF)
			next();
		break;
	default:
		error(&tok.loc, "invalid preprocessor directive #%s", tokenstr(tok.kind));
	}
	tokencheck(&tok, TNEWLINE, "after preprocessing directive");
	ppflags = oldflags;
}

/* get the next token without expanding it */
static void
nextinto(struct token *t)
{
	static bool newline = true;

	for (;;) {
		scan(t);
		if (newline && t->kind == THASH) {
			directive();
		} else {
			newline = tok.kind == TNEWLINE;
			break;
		}
	}
}

static struct token *
rawnext(void)
{
	struct token *t;

	t = ctxnext();
	if (!t) {
		t = &tok;
		nextinto(t);
	}
	return t;
}

static bool
peekparen(void)
{
	static struct array pending;
	struct token *t;
	struct frame *f;

	t = ctxnext();
	if (t) {
		if (t->kind == TLPAREN)
			return true;
		f = arraylast(&ctx, sizeof(*f));
		--f->token;
		++f->ntoken;
		return false;
	}
	pending.len = 0;
	do t = arrayadd(&pending, sizeof(*t)), nextinto(t);
	while (t->kind == TNEWLINE);
	if (t->kind == TLPAREN)
		return true;
	t = pending.val;
	ctxpush(t, pending.len / sizeof(*t), NULL, t[0].space);
	return false;
}

static void
stringize(struct array *buf, struct token *t)
{
	const char *lit;

	if ((t->space || t->kind == TNEWLINE) && buf->len > 1 && ((char *)buf->val)[buf->len - 1] != ' ')
		arrayaddbuf(buf, " ", 1);
	lit = t->lit ? t->lit : tokenstr(t->kind);
	if (t->kind == TSTRINGLIT || t->kind == TCHARCONST) {
		for (; *lit; ++lit) {
			if (*lit == '\\' || *lit == '"')
				arrayaddbuf(buf, "\\", 1);
			arrayaddbuf(buf, lit, 1);
		}
	} else if (lit) {
		arrayaddbuf(buf, lit, strlen(lit));
	}
}

static void expandfunc(struct macro *);

static bool
expand(struct token *t)
{
	struct macro *m;
	bool space;

	if (t->kind < TPPIDENT)
		return false;
	m = macroget(t->kind);
	if (!m || m->hide)
		t->hide = true;
	if (t->hide)
		return false;
	space = t->space;
	if (m->kind == MACROFUNC) {
		if (!peekparen())
			return false;
		expandfunc(m);
	}
	ctxpush(m->token, m->ntoken, m, space);
	m->hide = true;
	++macrodepth;
	return true;
}

static void
expandfunc(struct macro *m)
{
	struct macroparam *p;
	struct macroarg *arg;
	struct array str, tok;
	size_t i, depth, paren;
	struct token *t;

	/* read macro arguments */
	paren = 0;
	depth = macrodepth;
	tok = (struct array){0};
	arg = xreallocarray(NULL, m->nparam, sizeof(*arg));
	t = rawnext();
	for (i = 0; i < m->nparam; ++i) {
		p = &m->param[i];
		if (p->flags & PARAMSTR) {
			str = (struct array){0};
			arrayaddbuf(&str, "\"", 1);
		}
		arg[i].ntoken = 0;
		for (;;) {
			if (t->kind == TEOF)
				error(&t->loc, "EOF when reading macro parameters");
			if (macrodepth <= depth) {
				/* adjust current macro depth, in case it got shallower */
				depth = macrodepth;
				if (paren == 0 && (t->kind == TRPAREN || t->kind == TCOMMA && !(p->flags & PARAMVAR)))
					break;
				switch (t->kind) {
				case TLPAREN: ++paren; break;
				case TRPAREN: --paren; break;
				}
				if (p->flags & PARAMSTR)
					stringize(&str, t);
			}
			if (p->flags & PARAMTOK && !expand(t)) {
				arrayaddbuf(&tok, t, sizeof(*t));
				++arg[i].ntoken;
			}
			t = rawnext();
		}
		if (p->flags & PARAMSTR) {
			arrayaddbuf(&str, "\"", 2);
			arg[i].str = (struct token){
				.kind = TSTRINGLIT,
				.lit = str.val,
			};
		}
		if (t->kind == TRPAREN)
			break;
		t = rawnext();
	}
	if (i + 1 < m->nparam)
		error(&t->loc, "not enough arguments for macro '%s'", m->name);
	if (t->kind != TRPAREN)
		error(&t->loc, "too many arguments for macro '%s'", m->name);
	for (i = 0, t = tok.val; i < m->nparam; ++i) {
		arg[i].token = t;
		t += arg[i].ntoken;
	}
	m->arg = arg;
}

void
next(void)
{
	struct token *t;

	do t = rawnext();
	while (expand(t) || t->kind == TNEWLINE && !(ppflags & PPNEWLINE));
	tok = *t;

	/* keyword aliases */
	switch (tok.kind) {
	case ALIAS_ALIGNAS:       tok.kind = TALIGNAS;       break;
	case ALIAS_ALIGNOF:       tok.kind = TALIGNOF;       break;
	case ALIAS_BOOL:          tok.kind = TBOOL;          break;
	case ALIAS__INLINE:
	case ALIAS__INLINE__:     tok.kind = TINLINE;        break;
	case ALIAS__SIGNED:
	case ALIAS__SIGNED__:     tok.kind = TSIGNED;        break;
	case ALIAS_STATIC_ASSERT: tok.kind = TSTATIC_ASSERT; break;
	case ALIAS_THREAD_LOCAL:
	case ALIAS__THREAD:       tok.kind = TTHREAD_LOCAL;  break;
	case ALIAS__TYPEOF:
	case ALIAS__TYPEOF__:     tok.kind = TTYPEOF;        break;
	case ALIAS__VOLATILE__:   tok.kind = TVOLATILE;      break;
	case ALIAS__ASM:          tok.kind = T__ASM__;       break;
	}
}

bool
peek(enum tokenkind kind)
{
	static struct token pending;
	struct token old;

	old = tok;
	next();
	if (tok.kind == kind) {
		next();
		return true;
	}
	pending = tok;
	tok = old;
	ctxpush(&pending, 1, NULL, pending.space);
	return false;
}

char *
expect(enum tokenkind kind, const char *msg)
{
	char *lit;

	lit = tokencheck(&tok, kind, msg);
	next();

	return lit;
}

bool
consume(enum tokenkind kind)
{
	if (tok.kind != kind)
		return false;
	next();
	return true;
}
