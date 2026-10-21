#include "syma.h"
#include <ctype.h>
typedef struct {
    const char *p;
    Count pos;
    unsigned depth;
} Parser;
static void space(Parser *p) {
    for (;;) {
        while (p->p[p->pos] && (isspace((unsigned char)p->p[p->pos]) || p->p[p->pos] == ','))
            p->pos++;
        if (p->p[p->pos] == ';' || (p->p[p->pos] == '/' && p->p[p->pos + 1] == '/')) {
            while (p->p[p->pos] && p->p[p->pos] != '\n')
                p->pos++;
        } else if (p->p[p->pos] == '/' && p->p[p->pos + 1] == '*') {
            p->pos += 2;
            while (p->p[p->pos] && !(p->p[p->pos] == '*' && p->p[p->pos + 1] == '/'))
                p->pos++;
            if (!p->p[p->pos])
                syma_fail("unterminated block comment");
            p->pos += 2;
        } else
            break;
    }
}
static int hex(unsigned c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    syma_fail("invalid Unicode escape");
    return 0;
}
static Id stringlit(Parser *p, unsigned raw) {
    Buffer b = {0};
    Unit *u = NULL;
    Count n = 0, cap = 0;
    unsigned c, k, v;
    Id x;
    p->pos++;
    while ((c = (unsigned char)p->p[p->pos++]) != '"') {
        if (!c)
            syma_fail("unterminated string");
        if (!raw && c == '\\') {
            c = (unsigned char)p->p[p->pos++];
            if (!c)
                syma_fail("unterminated escape");
            if (c == 'n')
                c = '\n';
            else if (c == 'r')
                c = '\r';
            else if (c == 't')
                c = '\t';
            else if (c == 'b')
                c = 8;
            else if (c == 'f')
                c = 12;
            else if (c == 'u') {
                v = 0;
                for (k = 0; k < 4; k++) {
                    if (!p->p[p->pos])
                        syma_fail("short Unicode escape");
                    v = (v << 4) | (unsigned)hex((unsigned char)p->p[p->pos++]);
                }
                if (b.n) {
                    x = syma_utf8atom(STR, b.p, b.n);
                    if (n + syma_node(x)->len > cap) {
                        cap = n + syma_node(x)->len + 16;
                        u = syma_resize(u, cap * sizeof(Unit));
                    }
                    memcpy(u + n, syma_node(x)->s, syma_node(x)->len * sizeof(Unit));
                    n += syma_node(x)->len;
                    b.n = 0;
                }
                if (n == cap) {
                    cap = cap ? cap * 2 : 16;
                    u = syma_resize(u, cap * sizeof(Unit));
                }
                u[n++] = (Unit)v;
                continue;
            }
        }
        syma_bufput(&b, c);
    }
    if (b.n) {
        x = syma_utf8atom(STR, b.p, b.n);
        if (n + syma_node(x)->len > cap)
            u = syma_resize(u, (n + syma_node(x)->len) * sizeof(Unit));
        memcpy(u + n, syma_node(x)->s, syma_node(x)->len * sizeof(Unit));
        n += syma_node(x)->len;
    }
    x = syma_atom(STR, u, n);
    free(u);
    free(b.p);
    return x;
}
static int numeric_token(const char *p) {
    unsigned digits = 0;
    if (*p == 43 || *p == 45)
        p++;
    while (*p >= 48 && *p <= 57) {
        p++;
        digits++;
    }
    if (*p == 46) {
        p++;
        while (*p >= 48 && *p <= 57) {
            p++;
            digits++;
        }
    }
    if (!digits)
        return 0;
    if (*p == 101 || *p == 69) {
        p++;
        if (*p == 43 || *p == 45)
            p++;
        digits = 0;
        while (*p >= 48 && *p <= 57) {
            p++;
            digits++;
        }
        if (!digits)
            return 0;
    }
    return !*p;
}
static Id expr(Parser *p);
static Seq *group(Parser *p, unsigned end) {
    Seq *s = NULL;
    space(p);
    while (p->p[p->pos] && (unsigned char)p->p[p->pos] != end) {
        s = syma_cat(s, syma_one(expr(p)));
        space(p);
    }
    if ((unsigned char)p->p[p->pos] != end)
        syma_fail("missing closing delimiter");
    p->pos++;
    return s;
}
static Id expr(Parser *p) {
    Id x;
    Seq *s;
    Count start, n;
    char *t, *tail;
    double v;
    unsigned c;
    space(p);
    if (++p->depth > 256)
        syma_fail("parse nesting limit exceeded");
    c = (unsigned char)p->p[p->pos];
    if (c == '{') {
        p->pos++;
        s = group(p, '}');
        x = syma_fromflat(s);
    } else if (c == '(') {
        p->pos++;
        s = group(p, ')');
        x = syma_fromflat(s);
    } else if (c == '"')
        x = stringlit(p, 0);
    else if (c == 'r' && p->p[p->pos + 1] == '"') {
        p->pos++;
        x = stringlit(p, 1);
    } else {
        start = p->pos;
        while ((c = (unsigned char)p->p[p->pos]) && !isspace(c) && c != ',' && c != '{' &&
               c != '}' && c != '(' && c != ')' && c != ';')
            p->pos++;
        n = p->pos - start;
        if (!n)
            syma_fail("expected expression");
        t = syma_mem((n + 1) * sizeof(char));
        memcpy(t, p->p + start, n * sizeof(char));
        v = strtod(t, &tail);
        if (numeric_token(t) && tail != t && !*tail)
            x = syma_number(v);
        else if (!strcmp(t, "_") || !strcmp(t, "__"))
            x = syma_named("Var", syma_one(syma_str("_")));
        else if (!strcmp(t, "..") || !strcmp(t, "...") || !strcmp(t, "___"))
            x = syma_named("VarRest", syma_one(syma_str("_")));
        else if (n > 2 && t[n - 1] == '.' && t[n - 2] == '.') {
            t[n - (n > 3 && t[n - 3] == '.' ? 3 : 2)] = 0;
            x = syma_named("VarRest", syma_one(syma_str(t)));
        } else if (n > 1 && t[n - 1] == '_') {
            t[n - 1] = 0;
            x = syma_named("Var", syma_one(syma_str(t)));
        } else if (n > 2 && t[0] == '.' && t[1] == '.')
            x = syma_named("GreedyAnchor", syma_one(syma_sym(t + 2)));
        else
            x = syma_sym(t);
        free(t);
    }
    /* Only adjacent parentheses are function application; braces stay separate.
     */
    while (p->p[p->pos] == '(') {
        p->pos++;
        x = syma_call(x, group(p, ')'));
    }
    p->depth--;
    return x;
}
Id syma_parse(const char *text) {
    Parser p;
    Seq *s = NULL;
    p.p = text;
    p.pos = p.depth = 0;
    space(&p);
    while (text[p.pos]) {
        s = syma_cat(s, syma_one(expr(&p)));
        space(&p);
    }
    if (syma_length(s) == 1)
        return syma_at(s, 0);
    if (!s)
        return syma_call(0, NULL);
    return syma_named("Bundle", s);
}
static Id jexpr(Parser *p) {
    Id kind = 0, val = 0, h = 0, key;
    Seq *a = NULL;
    unsigned c;
    space(p);
    c = (unsigned char)p->p[p->pos];
    if (c == 'n' && !strncmp(p->p + p->pos, "null", 4)) {
        p->pos += 4;
        return 0;
    }
    if (c != '{')
        syma_fail("expected AST JSON object");
    p->pos++;
    space(p);
    while (p->p[p->pos] != '}') {
        if (p->p[p->pos] != '"')
            syma_fail("expected JSON key");
        key = stringlit(p, 0);
        space(p);
        if (p->p[p->pos++] != ':')
            syma_fail("expected colon");
        space(p);
        if (syma_is(key, "k"))
            kind = stringlit(p, 0);
        else if (syma_is(key, "v")) {
            if (p->p[p->pos] == '"')
                val = stringlit(p, 0);
            else {
                char *end;
                double n = strtod(p->p + p->pos, &end);
                if (end == p->p + p->pos)
                    syma_fail("invalid JSON number");
                p->pos = (Count)(end - p->p);
                val = syma_number(n);
            }
        } else if (syma_is(key, "h"))
            h = jexpr(p);
        else if (syma_is(key, "a")) {
            if (p->p[p->pos++] != '[')
                syma_fail("expected array");
            space(p);
            while (p->p[p->pos] != ']') {
                a = syma_cat(a, syma_one(jexpr(p)));
                space(p);
            }
            p->pos++;
        } else
            syma_fail("unknown AST field");
        space(p);
    }
    p->pos++;
    if (syma_is(kind, "Call"))
        return syma_call(h, a);
    if (syma_is(kind, "Num"))
        return val;
    if (!val)
        syma_fail("missing AST value");
    if (!syma_is(kind, "Sym") && !syma_is(kind, "Str"))
        syma_fail("invalid AST kind");
    return syma_atom(syma_is(kind, "Sym") ? SYM : STR, syma_node(val)->s, syma_node(val)->len);
}
Id syma_jsonparse(const char *text) {
    Parser p;
    p.p = text;
    p.pos = p.depth = 0;
    return jexpr(&p);
}
char *syma_readfile(const char *name) {
    FILE *f = fopen(name, "r");
    Buffer b = {0};
    int c;
    if (!f)
        syma_fail("cannot open file");
    while ((c = fgetc(f)) != EOF)
        syma_bufput(&b, (unsigned)c);
    fclose(f);
    return b.p ? b.p : syma_mem(1);
}

Id syma_inline_rule(char *text) {
    char *name = text, *p, *arrow = NULL, *rhs;
    Buffer b = {0};
    unsigned quote = 0, escape = 0;
    long depth = 0;
    while (*name == ' ' || *name == '\t')
        name++;
    p = name;
    while (*p && !isspace((unsigned char)*p))
        p++;
    if (!*p)
        syma_fail("expected rule name and pattern");
    *p++ = 0;
    rhs = p;
    while (*rhs) {
        unsigned c = (unsigned char)*rhs;
        if (quote) {
            if (escape)
                escape = 0;
            else if (c == '\\')
                escape = 1;
            else if (c == '"')
                quote = 0;
        } else if (c == '"')
            quote = 1;
        else if (c == '{' || c == '(')
            depth++;
        else if (c == '}' || c == ')')
            depth--;
        else if (!depth && c == '-' && rhs[1] == '>') {
            arrow = rhs;
            break;
        } else if (!depth && c == 0xe2 && (unsigned char)rhs[1] == 0x86 &&
                   (unsigned char)rhs[2] == 0x92) {
            arrow = rhs;
            break;
        }
        rhs++;
    }
    if (!arrow)
        syma_fail("expected -> between pattern and replacement");
    rhs = arrow + ((unsigned char)*arrow == 0xe2 ? 3 : 2);
    *arrow = 0;
    syma_buftext(&b, "{R ");
    char *quoted = syma_show(syma_str(name), 0);
    syma_buftext(&b, quoted);
    free(quoted);
    syma_bufput(&b, ' ');
    syma_buftext(&b, p);
    syma_bufput(&b, ' ');
    syma_buftext(&b, rhs);
    syma_buftext(&b, "\n}");
    Id result = syma_parse(b.p);
    free(b.p);
    return result;
}
