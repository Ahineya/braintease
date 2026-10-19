/* Portable C16 preprocessing. Uses cproc's scanner and token interning.
 * Token hide sets follow C macro rescanning rules; source directives are
 * consumed before expansion, so generated '#' tokens cannot become directives.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "util.h"
#include "cc.h"
#include "utf.h"

struct pphide {
    enum tokenkind name;
    struct pphide *next;
};

struct macro {
    bool function, variadic;
    enum tokenkind *params;
    size_t count;
    struct array body;
};

struct stream {
    struct token *tokens;
    size_t count, index;
    struct stream *next;
};

struct condition {
    bool parent, active, taken, otherwise;
    struct location loc;
};

struct input {
    const char *path;
    size_t base;
    bool bol;
    struct input *next;
};

struct expansion {
    struct stream *stack;
    bool source;
};
static struct array macros, includes, conditions, oncefiles;
static struct input *input;
static struct expansion source = {NULL, true};
enum ppflags ppflags;
static size_t expansiondepth, counter;

static size_t length(struct array *a)
{
    return a->len / sizeof(struct token);
}

static void add(struct array *a, struct token t)
{
    arrayaddbuf(a, &t, sizeof(t));
}

static char *copytext(const char *s)
{
    size_t n = strlen(s) + 1;
    char *r = xmalloc(n);
    memcpy(r, s, n);
    return r;
}

static const char *spelling(struct token t)
{
    return t.lit ? t.lit : tokenstr(t.kind);
}

static struct token number(unsigned long long n, struct location loc)
{
    char text[32];
    struct token t = {0};
    snprintf(text, sizeof(text), "%llu", n);
    t.kind = TNUMBER;
    t.loc = loc;
    t.lit = copytext(text);
    return t;
}

static struct macro *lookup(enum tokenkind name)
{
    return name >= TPPIDENT ? arraygetptr(&macros, name - TPPIDENT) : NULL;
}

static bool hidden(struct pphide *h, enum tokenkind name)
{
    for (; h; h = h->next)
        if (h->name == name)
            return true;
    return false;
}

static struct pphide *hideadd(struct pphide *h, enum tokenkind name)
{
    struct pphide *r;
    if (hidden(h, name))
        return h;
    r = xmalloc(sizeof(*r));
    r->name = name;
    r->next = h;
    return r;
}

static struct pphide *hideunion(struct pphide *a, struct pphide *b)
{
    for (; b; b = b->next)
        a = hideadd(a, b->name);
    return a;
}

static struct pphide *hideintersection(struct pphide *a, struct pphide *b)
{
    struct pphide *r = NULL;
    for (; a; a = a->next)
        if (hidden(b, a->name))
            r = hideadd(r, a->name);
    return r;
}

static void push(struct expansion *e, struct array a)
{
    struct stream *s;
    if (!a.len) {
        free(a.val);
        return;
    }
    s = xmalloc(sizeof(*s));
    s->tokens = a.val;
    s->count = length(&a);
    s->index = 0;
    s->next = e->stack;
    e->stack = s;
}

static void unread(struct expansion *e, struct token t)
{
    struct array a = {0};
    add(&a, t);
    push(e, a);
}
static struct token sourcenext(void);

static struct token raw(struct expansion *e)
{
    struct stream *s;
    struct token t = {0};
    while ((s = e->stack)) {
        if (s->index < s->count)
            return s->tokens[s->index++];
        e->stack = s->next;
        free(s->tokens);
        free(s);
    }
    if (e->source)
        return sourcenext();
    t.kind = TEOF;
    return t;
}
static struct token expanded(struct expansion *);

static struct array expandarray(struct array a)
{
    struct expansion e = {0};
    struct array r = {0};
    struct token t;
    struct array clone = {0};
    if (a.len)
        arrayaddbuf(&clone, a.val, a.len);
    push(&e, clone);
    while ((t = expanded(&e)).kind != TEOF)
        add(&r, t);
    return r;
}

static struct array lex(const char *text, struct location loc)
{
    struct array a = {0};
    struct token t;
    scanstring(text, loc);
    while (scan(&t), t.kind != TEOF)
        add(&a, t);
    scanpop();
    return a;
}

static struct token quoted(const char *s, struct location loc)
{
    struct array text = {0};
    struct token t = {0};
    arrayaddbuf(&text, "\"", 1);
    for (; *s; s++) {
        if (*s == '\\' || *s == '"')
            arrayaddbuf(&text, "\\", 1);
        arrayaddbuf(&text, s, 1);
    }
    arrayaddbuf(&text, "\"", 2);
    t.kind = TSTRINGLIT;
    t.lit = text.val;
    t.loc = loc;
    return t;
}

static struct token stringize(struct array a, struct location loc)
{
    struct array text = {0};
    struct token *t = a.val;
    size_t i;
    for (i = 0; i < length(&a); i++) {
        if (i && t[i].space)
            arrayaddbuf(&text, " ", 1);
        arrayaddbuf(&text, spelling(t[i]), strlen(spelling(t[i])));
    }
    arrayaddbuf(&text, "", 1);
    {
        struct token r = quoted(text.val, loc);
        free(text.val);
        return r;
    }
}

static size_t parameter(struct macro *m, enum tokenkind name)
{
    size_t i;
    for (i = 0; i < m->count; i++)
        if (m->params[i] == name)
            return i;
    return (size_t)-1;
}

static void macrofree(struct macro *m)
{
    if (m) {
        free(m->params);
        free(m->body.val);
        free(m);
    }
}

static void define(struct array a)
{
    struct token *t = a.val;
    struct macro *m, *old;
    struct array params = {0};
    size_t n = length(&a), i = 1, j;
    if (!n || t[0].kind < TPPIDENT)
        error(n ? &t[0].loc : &tok.loc, "expected macro name");
    m = xmalloc(sizeof(*m));
    memset(m, 0, sizeof(*m));
    if (i < n && t[i].kind == TLPAREN && !t[i].space) {
        m->function = true;
        i++;
        while (i < n && t[i].kind != TRPAREN) {
            enum tokenkind p = t[i].kind;
            if (p == TELLIPSIS) {
                p = T__VA_ARGS__;
                m->variadic = true;
            } else if (p < TPPIDENT)
                error(&t[i].loc, "expected macro parameter");
            for (j = 0; j < m->count; j++)
                if (((enum tokenkind *)params.val)[j] == p)
                    error(&t[i].loc, "duplicate macro parameter");
            arrayaddbuf(&params, &p, sizeof(p));
            m->count++;
            i++;
            if (i < n && t[i].kind == TRPAREN)
                break;
            if (m->variadic || i >= n || t[i].kind != TCOMMA)
                error(&t[i - 1].loc, "expected ')' or ',' after macro parameter");
            i++;
            if (i < n && t[i].kind == TRPAREN)
                error(&t[i].loc, "expected macro parameter");
        }
        if (i >= n)
            error(&t[0].loc, "unterminated macro parameters");
        i++;
    }
    m->params = params.val;
    for (j = i; j < n; j++) {
        if (t[j].kind == THASHHASH && (j == i || j + 1 == n))
            error(&t[j].loc, "'##' cannot start or end a macro");
        if (m->function && t[j].kind == THASH &&
            (j + 1 == n || parameter(m, t[j + 1].kind) == (size_t)-1))
            error(&t[j].loc, "'#' requires a macro parameter");
        if (t[j].kind == T__VA_ARGS__ && !m->variadic)
            error(&t[j].loc, "__VA_ARGS__ requires a variadic macro");
        add(&m->body, t[j]);
    }
    old = arraysetptr(&macros, t[0].kind - TPPIDENT, m);
    if (old) {
        bool equal = old->function == m->function && old->variadic == m->variadic &&
                     old->count == m->count && old->body.len == m->body.len;
        for (j = 0; equal && j < m->count; j++)
            equal = old->params[j] == m->params[j];
        for (j = 0; equal && j < length(&m->body); j++) {
            struct token *x = old->body.val, *y = m->body.val;
            equal = x[j].kind == y[j].kind && !strcmp(spelling(x[j]), spelling(y[j]));
        }
        if (!equal) {
            diagloc(&t[0].loc);
            fprintf(stderr, "warning: redefinition of macro '%s'\n", spelling(t[0]));
        }
        macrofree(old);
    }
}

void ppinclude(const char *path)
{
    arrayaddptr(&includes, (void *)path);
}

void ppdefine(const char *text)
{
    char *s = copytext(text), *equals = strchr(s, '=');
    struct array a;
    struct location loc = {"<command-line>", 1, 1};
    if (equals)
        *equals = ' ';
    else {
        free(s);
        s = xmalloc(strlen(text) + 3);
        sprintf(s, "%s 1", text);
    }
    a = lex(s, loc);
    define(a);
    free(a.val);
    free(s);
}

void ppundef(const char *name)
{
    enum tokenkind k = tokenget(name, strlen(name));
    struct macro *m = lookup(k);
    if (m) {
        arraysetptr(&macros, k - TPPIDENT, NULL);
        macrofree(m);
    }
}

static void replacement(struct expansion *e, struct macro *m, struct token invocation)
{
    struct array *args = NULL, *values = NULL, body = {0}, pasted = {0};
    struct token t, closing = invocation, *b = m->body.val;
    size_t i, j, index, depth = 0, count = 0;
    struct pphide *h;
    if (m->function) {
        struct array current = {0};
        size_t capacity = m->count ? m->count : 1;
        args = xmalloc(capacity * sizeof(*args));
        memset(args, 0, capacity * sizeof(*args));
        {
            bool newline = false;
            for (;;) {
                t = raw(e);
                if (t.kind == TEOF)
                    error(&invocation.loc, "unterminated arguments for macro '%s'",
                          spelling(invocation));
                if (t.kind == TNEWLINE) {
                    newline = true;
                    continue;
                }
                if (newline) {
                    t.space = true;
                    newline = false;
                }
                if (!depth && (t.kind == TRPAREN ||
                               (t.kind == TCOMMA && (!m->variadic || count + 1 < m->count)))) {
                    if (count >= capacity)
                        error(&invocation.loc, "too many arguments for macro '%s'",
                              spelling(invocation));
                    args[count++] = current;
                    current = (struct array){0};
                    if (t.kind == TRPAREN) {
                        closing = t;
                        break;
                    }
                } else {
                    if (t.kind == TLPAREN)
                        depth++;
                    if (t.kind == TRPAREN)
                        depth--;
                    add(&current, t);
                }
            }
        }
        if (!m->count && count == 1 && !args[0].len)
            count = 0;
        /* C permits omission of the sole variadic argument in this bootstrap. */
        if (m->variadic && count + 1 == m->count)
            count++;
        if (count != m->count)
            error(&invocation.loc, "wrong number of arguments for macro '%s'",
                  spelling(invocation));
        values = xmalloc(capacity * sizeof(*values));
        memset(values, 0, capacity * sizeof(*values));
        for (i = 0; i < m->count; i++) {
            bool used = false;
            for (j = 0; j < length(&m->body); j++)
                if (b[j].kind == m->params[i] && !(j && b[j - 1].kind == THASH) &&
                    !(j && b[j - 1].kind == THASHHASH) &&
                    !(j + 1 < length(&m->body) && b[j + 1].kind == THASHHASH))
                    used = true;
            if (used)
                values[i] = expandarray(args[i]);
        }
    }
    h = hideadd(m->function ? hideintersection(invocation.pphide, closing.pphide)
                            : invocation.pphide,
                invocation.kind);
    for (i = 0; i < length(&m->body); i++) {
        t = b[i];
        t.loc = invocation.loc;
        if (m->function && t.kind == THASH) {
            index = parameter(m, b[++i].kind);
            t = stringize(args[index], invocation.loc);
            add(&body, t);
            continue;
        }
        index = m->function ? parameter(m, t.kind) : (size_t)-1;
        if (index != (size_t)-1) {
            bool paste = (i && b[i - 1].kind == THASHHASH) ||
                         (i + 1 < length(&m->body) && b[i + 1].kind == THASHHASH);
            struct array a = paste ? args[index] : values[index];
            struct token *v = a.val;
            if (!a.len && paste) {
                t.kind = TNONE;
                t.lit = NULL;
                add(&body, t);
            }
            for (j = 0; j < length(&a); j++) {
                struct token u = v[j];
                if (!j)
                    u.space = t.space;
                add(&body, u);
            }
        } else
            add(&body, t);
    }
    b = body.val;
    for (i = 0; i < length(&body); i++) {
        t = b[i];
        if (t.kind == THASHHASH) {
            struct token *left = arraylast(&pasted, sizeof(t)), right = b[++i];
            if (left->kind == TNONE) {
                bool space = left->space;
                *left = right;
                left->space = space;
            } else if (right.kind != TNONE) {
                struct array text = {0}, tokens;
                const char *a = spelling(*left), *c = spelling(right);
                arrayaddbuf(&text, a, strlen(a));
                arrayaddbuf(&text, c, strlen(c) + 1);
                tokens = lex(text.val, invocation.loc);
                if (length(&tokens) != 1)
                    error(&invocation.loc, "invalid token paste '%s'", (char *)text.val);
                {
                    struct token merged = ((struct token *)tokens.val)[0];
                    merged.space = left->space;
                    merged.pphide = hideintersection(left->pphide, right.pphide);
                    *left = merged;
                }
                free(tokens.val);
                free(text.val);
            }
        } else
            add(&pasted, t);
    }
    free(body.val);
    body = (struct array){0};
    b = pasted.val;
    for (i = 0; i < length(&pasted); i++)
        if (b[i].kind != TNONE) {
            t = b[i];
            t.pphide = hideunion(t.pphide, h);
            add(&body, t);
        }
    if (body.len)
        ((struct token *)body.val)[0].space = invocation.space;
    free(pasted.val);
    for (i = 0; i < m->count; i++) {
        free(args[i].val);
        free(values[i].val);
    }
    free(args);
    free(values);
    push(e, body);
}

static struct token expanded(struct expansion *e)
{
    struct token t, u;
    struct macro *m;
    const char *name;
    if (++expansiondepth > 256)
        fatal("macro argument expansion nesting exceeds 256");
    for (;;) {
        t = raw(e);
        if (t.kind < TPPIDENT)
            break;
        name = spelling(t);
        if (!strcmp(name, "__LINE__")) {
            u = number(t.loc.line, t.loc);
            u.space = t.space;
            t = u;
            break;
        }
        if (!strcmp(name, "__FILE__")) {
            u = quoted(t.loc.file, t.loc);
            u.space = t.space;
            t = u;
            break;
        }
        if (!strcmp(name, "__COUNTER__")) {
            u = number(counter++, t.loc);
            u.space = t.space;
            t = u;
            break;
        }
        m = lookup(t.kind);
        if (!m || hidden(t.pphide, t.kind))
            break;
        if (m->function) {
            do
                u = raw(e);
            while (u.kind == TNEWLINE);
            if (u.kind != TLPAREN) {
                unread(e, u);
                break;
            }
        }
        replacement(e, m, t);
    }
    expansiondepth--;
    return t;
}

/* Preprocessor integer expressions use intmax_t/uintmax_t semantics, rather
 * than the target's 16-bit int. Store bits unsigned to avoid host signed UB. */
struct integer {
    unsigned long long bits;
    bool unsign;
};

struct expression {
    struct token *t;
    size_t n, i;
    struct location loc;
};
static struct integer expression(struct expression *, int, bool);

static int precedence(enum tokenkind k)
{
    switch (k) {
    case TLOR:
        return 1;
    case TLAND:
        return 2;
    case TBOR:
        return 3;
    case TXOR:
        return 4;
    case TBAND:
        return 5;
    case TEQL:
    case TNEQ:
        return 6;
    case TLESS:
    case TLEQ:
    case TGREATER:
    case TGEQ:
        return 7;
    case TSHL:
    case TSHR:
        return 8;
    case TADD:
    case TSUB:
        return 9;
    case TMUL:
    case TDIV:
    case TMOD:
        return 10;
    default:
        return 0;
    }
}

static unsigned long long character(const char *s, struct location loc)
{
    unsigned long long v = 0;
    int c, n;
    size_t count = 0;
    if (*s != '\'')
        error(&loc, "prefixed #if character constants are not implemented");
    s++;
    while (*s && *s != '\'') {
        c = (unsigned char)*s++;
        if (c == '\\') {
            c = *s++;
            switch (c) {
            case 'a':
                c = 7;
                break;
            case 'b':
                c = 8;
                break;
            case 'f':
                c = 12;
                break;
            case 'n':
                c = 10;
                break;
            case 'r':
                c = 13;
                break;
            case 't':
                c = 9;
                break;
            case 'v':
                c = 11;
                break;
            default:
                if (c == 'x') {
                    char *end;
                    v = strtoull(s, &end, 16);
                    if (v > 65535)
                        error(&loc, "#if character escape is out of range");
                    c = (int)v;
                    s = end;
                } else if (c >= '0' && c <= '7') {
                    c -= '0';
                    for (n = 1; n < 3 && *s >= '0' && *s <= '7'; n++)
                        c = c * 8 + *s++ - '0';
                }
            }
        } else if ((unsigned)c >= 128) {
            uint_least32_t code;
            size_t len = utf8dec(&code, (const unsigned char *)s - 1, strlen(s) + 1);
            if (len == (size_t)-1 || code > 65535)
                error(&loc, "invalid C16 #if character");
            s += len - 1;
            c = (int)code;
        }
        v = (unsigned)c & 65535U;
        count++;
    }
    if (count != 1)
        error(&loc, "#if character constant must contain one character");
    return (v ^ 0x8000ULL) - 0x8000ULL;
}

static struct integer primary(struct expression *e, bool evaluate)
{
    struct integer v = {0, false};
    struct token t;
    char *end;
    int k;
    if (e->i == e->n)
        error(&e->loc, "expected #if expression");
    t = e->t[e->i++];
    k = t.kind;
    if (k == TLPAREN) {
        v = expression(e, 0, evaluate);
        if (e->i == e->n || e->t[e->i++].kind != TRPAREN)
            error(&t.loc, "expected ')' in #if");
    } else if (k == TADD || k == TSUB || k == TLNOT || k == TBNOT) {
        v = primary(e, evaluate);
        if (k == TSUB)
            v.bits = 0 - v.bits;
        if (k == TBNOT)
            v.bits = ~v.bits;
        if (k == TLNOT) {
            v.bits = !v.bits;
            v.unsign = false;
        }
    } else if (k == TNUMBER) {
        errno = 0;
        v.bits = strtoull(t.lit, &end, 0);
        if (end == t.lit || errno == ERANGE)
            error(&t.loc, "invalid #if integer '%s'", t.lit);
        while (*end == 'u' || *end == 'U' || *end == 'l' || *end == 'L') {
            if (*end == 'u' || *end == 'U')
                v.unsign = true;
            end++;
        }
        if (*end)
            error(&t.loc, "invalid #if integer '%s'", t.lit);
        if (v.bits > 0x7fffffffffffffffULL)
            v.unsign = true;
    } else if (k == TCHARCONST)
        v.bits = character(t.lit, t.loc);
    else if (k >= TPPIDENT)
        v.bits = 0;
    else
        error(&t.loc, "invalid #if expression");
    return v;
}

static struct integer expression(struct expression *e, int min, bool evaluate)
{
    struct integer a = primary(e, evaluate), b, c;
    int p;
    enum tokenkind op;
    bool u;
    unsigned long long x, y;
    while (e->i < e->n && (p = precedence(e->t[e->i].kind)) > min) {
        op = e->t[e->i++].kind;
        b = expression(e, p, evaluate && !(op == TLAND && !a.bits) && !(op == TLOR && a.bits));
        x = a.bits;
        y = b.bits;
        u = a.unsign || b.unsign;
        switch (op) {
        case TADD:
            a.bits = x + y;
            break;
        case TSUB:
            a.bits = x - y;
            break;
        case TMUL:
            a.bits = x * y;
            break;
        case TDIV:
        case TMOD:
            if (!evaluate) {
                a.bits = 0;
                break;
            }
            if (!y)
                error(&e->loc, "division by zero in #if");
            if (u)
                a.bits = op == TDIV ? x / y : x % y;
            else {
                bool nx = (long long)x < 0, ny = (long long)y < 0;
                unsigned long long ax = nx ? 0 - x : x, ay = ny ? 0 - y : y;
                a.bits = op == TDIV ? ax / ay : ax % ay;
                if (op == TDIV ? nx != ny : nx)
                    a.bits = 0 - a.bits;
            }
            break;
        case TSHL:
        case TSHR:
            if (evaluate && y >= 64)
                error(&e->loc, "invalid shift in #if");
            y &= 63;
            a.bits = op == TSHL ? x << y : x >> y;
            if (op == TSHR && !a.unsign && (long long)x < 0 && y)
                a.bits |= ~0ULL << (64 - y);
            u = a.unsign;
            break;
        case TBAND:
            a.bits = x & y;
            break;
        case TBOR:
            a.bits = x | y;
            break;
        case TXOR:
            a.bits = x ^ y;
            break;
        case TLESS:
            a.bits = u ? x < y : (long long)x < (long long)y;
            u = false;
            break;
        case TLEQ:
            a.bits = u ? x <= y : (long long)x <= (long long)y;
            u = false;
            break;
        case TGREATER:
            a.bits = u ? x > y : (long long)x > (long long)y;
            u = false;
            break;
        case TGEQ:
            a.bits = u ? x >= y : (long long)x >= (long long)y;
            u = false;
            break;
        case TEQL:
            a.bits = x == y;
            u = false;
            break;
        case TNEQ:
            a.bits = x != y;
            u = false;
            break;
        case TLAND:
            a.bits = x && y;
            u = false;
            break;
        case TLOR:
            a.bits = x || y;
            u = false;
            break;
        default:
            break;
        }
        a.unsign = u;
    }
    if (min == 0 && e->i < e->n && e->t[e->i].kind == TQUESTION) {
        e->i++;
        b = expression(e, 0, evaluate && a.bits);
        if (e->i == e->n || e->t[e->i++].kind != TCOLON)
            error(&e->loc, "expected ':' in #if");
        c = expression(e, 0, evaluate && !a.bits);
        a.bits = a.bits ? b.bits : c.bits;
        a.unsign = b.unsign || c.unsign;
    }
    return a;
}

static bool builtin(enum tokenkind k)
{
    const char *s = k >= TPPIDENT ? tokenstr(k) : "";
    return !strcmp(s, "__FILE__") || !strcmp(s, "__LINE__") || !strcmp(s, "__COUNTER__");
}

static bool test(struct array a, struct location loc)
{
    struct array resolved = {0}, out;
    struct token *t = a.val, v;
    size_t i;
    struct expression e;
    struct integer result;
    for (i = 0; i < length(&a); i++) {
        if (t[i].kind >= TPPIDENT && !strcmp(spelling(t[i]), "defined")) {
            bool paren = false, yes;
            if (++i < length(&a) && t[i].kind == TLPAREN) {
                paren = true;
                i++;
            }
            if (i == length(&a) || t[i].kind < TPPIDENT)
                error(&loc, "defined requires an identifier");
            yes = lookup(t[i].kind) != NULL || builtin(t[i].kind);
            if (paren && (++i == length(&a) || t[i].kind != TRPAREN))
                error(&loc, "expected ')' after defined");
            v = number(yes, loc);
            add(&resolved, v);
        } else
            add(&resolved, t[i]);
    }
    out = expandarray(resolved);
    free(resolved.val);
    e.t = out.val;
    e.n = length(&out);
    e.i = 0;
    e.loc = loc;
    result = expression(&e, 0, true);
    if (e.i != e.n)
        error(&e.t[e.i].loc, "trailing tokens in #if");
    free(out.val);
    return result.bits != 0;
}

static bool active(void)
{
    struct condition *c = arraylast(&conditions, sizeof(*c));
    return !conditions.len || c->active;
}

static char *join(const char *directory, size_t n, const char *name)
{
    char *path = xmalloc(n + strlen(name) + 2);
    memcpy(path, directory, n);
    path[n] = '/';
    strcpy(path + n + 1, name);
    return path;
}

static void openinclude(struct array a, struct location loc)
{
    struct array out = {0}, name = {0};
    struct token *t = a.val;
    size_t i, n = length(&a);
    bool quotedname = false;
    char *path = NULL;
    FILE *file = NULL;
    if (!n || (t[0].kind != TSTRINGLIT && t[0].kind != TLESS)) {
        out = expandarray(a);
        t = out.val;
        n = length(&out);
    }
    if (n == 1 && t[0].kind == TSTRINGLIT) {
        const char *s = t[0].lit;
        size_t len = strlen(s);
        if (s[0] != '"' || len < 2)
            error(&loc, "invalid include path");
        arrayaddbuf(&name, s + 1, len - 2);
        quotedname = true;
    } else if (n >= 2 && t[0].kind == TLESS && t[n - 1].kind == TGREATER) {
        for (i = 1; i + 1 < n; i++)
            arrayaddbuf(&name, spelling(t[i]), strlen(spelling(t[i])));
    } else
        error(&loc, "expected a header name after #include");
    arrayaddbuf(&name, "", 1);
    if (((char *)name.val)[0] == '/') {
        path = copytext(name.val);
        file = fopen(path, "r");
    } else {
        if (quotedname) {
            const char *slash = strrchr(input->path, '/');
            path = slash ? join(input->path, (size_t)(slash - input->path), name.val)
                         : copytext(name.val);
            file = fopen(path, "r");
            if (!file) {
                free(path);
                path = NULL;
            }
        }
        for (i = 0; !file && i < includes.len / sizeof(void *); i++) {
            const char *dir = ((const char **)includes.val)[i];
            path = join(dir, strlen(dir), name.val);
            file = fopen(path, "r");
            if (!file) {
                free(path);
                path = NULL;
            }
        }
    }
    if (!file)
        error(&loc, "cannot find include '%s'", (char *)name.val);
    for (i = 0; i < oncefiles.len / sizeof(void *); i++)
        if (!strcmp(path, ((char **)oncefiles.val)[i])) {
            fclose(file);
            free(path);
            file = NULL;
            break;
        }
    if (file) {
        struct input *s, *p;
        size_t depth = 0;
        for (p = input; p; p = p->next)
            depth++;
        if (depth >= 128)
            error(&loc, "include nesting exceeds 128");
        s = xmalloc(sizeof(*s));
        s->path = path;
        s->base = conditions.len;
        s->bol = true;
        s->next = input;
        input = s;
        scanfrom(path, file);
    }
    free(out.val);
    free(name.val);
}

static void directive(struct token hash)
{
    struct token t, *v;
    struct array line = {0}, a = {0}, out;
    size_t n, i;
    enum tokenkind k;
    struct condition *c;
    while (scan(&t), t.kind != TNEWLINE && t.kind != TEOF)
        add(&line, t);
    input->bol = true;
    v = line.val;
    n = length(&line);
    if (!n) {
        free(line.val);
        return;
    }
    k = v[0].kind;
    if (n > 1)
        arrayaddbuf(&a, v + 1, (n - 1) * sizeof(*v));
    if (k == TIF || k == TIFDEF || k == TIFNDEF) {
        bool parent = active(), yes = false;
        if (parent) {
            if (k == TIF)
                yes = test(a, hash.loc);
            else {
                if (n != 2 || v[1].kind < TPPIDENT)
                    error(&hash.loc, "expected one identifier after conditional");
                yes = lookup(v[1].kind) != NULL || builtin(v[1].kind);
                if (k == TIFNDEF)
                    yes = !yes;
            }
        }
        c = arrayadd(&conditions, sizeof(*c));
        c->parent = parent;
        c->active = parent && yes;
        c->taken = yes;
        c->otherwise = false;
        c->loc = hash.loc;
    } else if (k == TELIF || k == TELSE || k == TENDIF) {
        if (conditions.len == input->base)
            error(&hash.loc, "unmatched conditional directive");
        c = arraylast(&conditions, sizeof(*c));
        if (k == TENDIF) {
            if (n != 1)
                error(&hash.loc, "tokens after #endif");
            conditions.len -= sizeof(*c);
        } else {
            if (c->otherwise)
                error(&hash.loc, "conditional after #else");
            if (k == TELSE) {
                if (n != 1)
                    error(&hash.loc, "tokens after #else");
                c->otherwise = true;
            }
            c->active = c->parent && !c->taken && (k == TELSE || test(a, hash.loc));
            c->taken = c->taken || c->active;
        }
    } else if (active())
        switch (k) {
        case TDEFINE:
            define(a);
            break;
        case TUNDEF:
            if (n != 2 || v[1].kind < TPPIDENT)
                error(&hash.loc, "expected one identifier after #undef");
            ppundef(spelling(v[1]));
            break;
        case TINCLUDE:
            openinclude(a, hash.loc);
            break;
        case TPRAGMA:
            if (n == 2 && v[1].kind >= TPPIDENT && !strcmp(spelling(v[1]), "once"))
                arrayaddptr(&oncefiles, (void *)input->path);
            break;
        case TLINE:
        case TNUMBER:
            if (k == TNUMBER) {
                free(a.val);
                a = (struct array){0};
                arrayaddbuf(&a, line.val, line.len);
            }
            out = expandarray(a);
            v = out.val;
            n = length(&out);
            if (!n || v[0].kind != TNUMBER)
                error(&hash.loc, "expected line number");
            {
                struct location loc = hash.loc;
                char *end;
                unsigned long long value = strtoull(v[0].lit, &end, 10);
                if (*end || !value || value > 2147483647UL)
                    error(&hash.loc, "invalid line number");
                loc.line = (size_t)value;
                loc.col = 1;
                if (n > 1 && v[1].kind == TSTRINGLIT) {
                    const char *s = v[1].lit;
                    struct array file = {0};
                    if (*s++ != '"')
                        error(&hash.loc, "invalid #line filename");
                    while (*s && *s != '"') {
                        if (*s == '\\' && (s[1] == '\\' || s[1] == '"'))
                            s++;
                        arrayaddbuf(&file, s++, 1);
                    }
                    arrayaddbuf(&file, "", 1);
                    loc.file = file.val;
                } else if (n > 1 && k == TLINE)
                    error(&hash.loc, "invalid #line filename");
                for (i = (n > 1 && v[1].kind == TSTRINGLIT) ? 2 : 1; i < n; i++)
                    if (k == TLINE || v[i].kind != TNUMBER)
                        error(&hash.loc, "tokens after #line");
                scansetloc(loc);
            }
            free(out.val);
            break;
        case TERROR:
        case TWARNING:
            diagloc(&hash.loc);
            fprintf(stderr, "%s:", k == TERROR ? "#error" : "#warning");
            for (i = 0; i < length(&a); i++) {
                fputc(' ', stderr);
                fputs(spelling(((struct token *)a.val)[i]), stderr);
            }
            fputc('\n', stderr);
            if (k == TERROR)
                exit(1);
            break;
        default:
            error(&hash.loc, "invalid preprocessing directive");
        }
    free(a.val);
    free(line.val);
}

static struct token sourcenext(void)
{
    struct token t;
    bool bol;
    for (;;) {
        scan(&t);
        if (t.kind == TEOF) {
            if (conditions.len != input->base) {
                struct condition *c = arraylast(&conditions, sizeof(*c));
                error(&c->loc, "unterminated conditional directive");
            }
            if (!input->next)
                return t;
            {
                struct input *old = input;
                input = old->next;
                free(old);
                scanpop();
            }
            continue;
        }
        bol = input->bol;
        input->bol = t.kind == TNEWLINE;
        if (bol && t.kind == THASH) {
            directive(t);
            continue;
        }
        if (active())
            return t;
    }
}

void ppinit(void)
{
    /* The scanner's logical filename may change with #line; include lookup
     * retains the physical filename separately. Driver installs this value. */
    next();
}

void ppstart(const char *path)
{
    input = xmalloc(sizeof(*input));
    input->path = path;
    input->base = 0;
    input->bol = true;
    input->next = NULL;
}

void next(void)
{
    do
        tok = expanded(&source);
    while (tok.kind == TNEWLINE && !(ppflags & PPNEWLINE));
    if (ppflags & 2)
        return;
    switch (tok.kind) {
    case ALIAS_ALIGNAS:
        tok.kind = TALIGNAS;
        break;
    case ALIAS_ALIGNOF:
        tok.kind = TALIGNOF;
        break;
    case ALIAS_BOOL:
        tok.kind = TBOOL;
        break;
    case ALIAS__INLINE:
    case ALIAS__INLINE__:
        tok.kind = TINLINE;
        break;
    case ALIAS__SIGNED:
    case ALIAS__SIGNED__:
        tok.kind = TSIGNED;
        break;
    case ALIAS_STATIC_ASSERT:
        tok.kind = TSTATIC_ASSERT;
        break;
    case ALIAS_THREAD_LOCAL:
    case ALIAS__THREAD:
        tok.kind = TTHREAD_LOCAL;
        break;
    case ALIAS__TYPEOF:
    case ALIAS__TYPEOF__:
        tok.kind = TTYPEOF;
        break;
    case ALIAS__VOLATILE__:
        tok.kind = TVOLATILE;
        break;
    case ALIAS__ASM:
        tok.kind = T__ASM__;
        break;
    default:
        break;
    }
}

bool peek(enum tokenkind kind)
{
    struct token old = tok;
    next();
    if (tok.kind == kind) {
        next();
        return true;
    }
    unread(&source, tok);
    tok = old;
    return false;
}

char *expect(enum tokenkind kind, const char *message)
{
    char *lit = tokencheck(&tok, kind, message);
    next();
    return lit;
}

bool consume(enum tokenkind kind)
{
    if (tok.kind != kind)
        return false;
    next();
    return true;
}

void ppoutput(FILE *file)
{
    const char *lastfile = NULL;
    size_t lastline = 0;
    /* Print a separator between every token. This preserves token boundaries
     * even for adjacent '+' '+' or '/' '*' created by expansion. */
    for (; tok.kind != TEOF; next()) {
        if (tok.kind == TNEWLINE)
            continue;
        if (!lastfile || strcmp(lastfile, tok.loc.file) || lastline != tok.loc.line) {
            struct token filename = quoted(tok.loc.file, tok.loc);
            fprintf(file, "\n#line %zu %s\n", tok.loc.line, filename.lit);
            free(filename.lit);
            lastfile = tok.loc.file;
            lastline = tok.loc.line;
        }
        fputs(spelling(tok), file);
        fputc(' ', file);
    }
    fputc('\n', file);
}
