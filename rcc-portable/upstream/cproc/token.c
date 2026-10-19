#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"
#include "cc.h"

struct token tok;
static struct map tokmap;  /* maps string to token */
static struct array tokstr;  /* maps token to string */

void
tokeninit(void)
{
	static const unsigned char strings[] =
#define TOKEN(t, s) s "\0"
#include "tokens.h"
#undef TOKEN
		;
	const unsigned char *s, *e;
	struct mapkey k;
	size_t l, i;

	mapinit(&tokmap, 1024);
	for (s = strings, e = strings + sizeof strings - 1; s != e; s += l + 1) {
		l = strlen((char *)s);
		if (l) {
			mapkey(&k, s, l);
			mapput(&tokmap, &k, &i);
			tokmap.vals[i].i = tokstr.len / sizeof(void *);
			arrayaddptr(&tokstr, (void *)s);
		} else {
			arrayaddptr(&tokstr, NULL);
		}
	}
}

int
tokenget(const void *str, size_t len)
{
	static char *pos, *end;
	struct mapkey k;
	size_t i;
	char *buf;

	mapkey(&k, str, len);
	if (mapput(&tokmap, &k, &i)) {
		if (len > 8192)
			fatal("token is too long");
		if (INT_MAX < tokstr.len / sizeof(void *))
			fatal("too many tokens");
		if (!pos || end - pos < len + 1) {
			buf = xmalloc(8192);
			pos = buf;
			end = buf + 8192;
		}
		memcpy(pos, str, len);
		pos[len] = '\0';
		tokmap.keys[i].str = pos;
		tokmap.vals[i].i = tokstr.len / sizeof(void *);
		arrayaddptr(&tokstr, pos);
		pos += len + 1;
	}
	return tokmap.vals[i].i;
}

char *
tokenstr(enum tokenkind kind)
{
	assert(kind < tokstr.len / sizeof(void *));
	return ((void **)tokstr.val)[kind];
}

void
tokenprint(const struct token *t, FILE *f)
{
	const char *str;

	if (t->space)
		fputc(' ', f);
	switch (t->kind) {
	case TNUMBER:
	case TCHARCONST:
	case TSTRINGLIT:
		str = t->lit;
		break;
	case TNEWLINE:
		str = "\n";
		break;
	case TEOF:
		return;
	default:
		str = tokenstr(t->kind);
	}
	if (!str)
		fatal("cannot print token %d", t->kind);
	fputs(str, f);
}

static void
tokendesc(char *buf, size_t len, enum tokenkind kind, const char *lit)
{
	const char *class;
	bool quote = true;

	assert(kind < tokstr.len / sizeof(void *));
	switch (kind) {
	case TEOF:       class = "EOF";                       break;
	case TNUMBER:    class = "number";     quote = true;  break;
	case TCHARCONST: class = "character";  quote = false; break;
	case TSTRINGLIT: class = "string";     quote = false; break;
	case TNEWLINE:   class = "newline";                   break;
	case TOTHER:     class = NULL;                        break;
	default:
		if (kind >= TIDENT)
			class = "identifier", quote = true;
		else
			class = NULL;
		lit = ((void **)tokstr.val)[kind];
	}
	if (class && lit)
		snprintf(buf, len, quote ? "%s '%s'" : "%s %s", class, lit);
	else if (class)
		snprintf(buf, len, "%s", class);
	else if (kind == TOTHER && !isprint(*(unsigned char *)lit))
		snprintf(buf, len, "<U+%04x>", *(unsigned char *)lit);
	else if (lit)
		snprintf(buf, len, "'%s'", lit);
	else
		snprintf(buf, len, "<unknown>");
}

char *
tokencheck(const struct token *t, enum tokenkind kind, const char *msg)
{
	char want[64], got[64];

	if (kind == TPPIDENT || kind == TIDENT) {
		if (t->kind < kind) {
			strcpy(want, "identifier");
			goto err;
		}
		return tokenstr(t->kind);
	}
	if (t->kind != kind && (kind != TIDENT || t->kind < TIDENT)) {
		tokendesc(want, sizeof(want), kind, NULL);
	err:
		tokendesc(got, sizeof(got), t->kind, t->lit);
		error(&t->loc, "expected %s %s, saw %s", want, msg, got);
	}
	return t->lit;
}

void
diagloc(const struct location *loc)
{
	fprintf(stderr, "%s:%zu:%zu: ", loc->file, loc->line, loc->col);
}

void
error(const struct location *loc, const char *fmt, ...)
{
	va_list ap;

	diagloc(loc);
	fputs("error: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	putc('\n', stderr);
	exit(1);
}
