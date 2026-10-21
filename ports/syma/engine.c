#include "syma.h"
#include "libregexp.h"
#include <limits.h>
#include <stddef.h>
#if CHAR_BIT == 16
typedef long QInt;
typedef qjs_octet_t Octet;
#else
typedef int QInt;
typedef uint8_t Octet;
#endif
void *lre_realloc(void *opaque, void *ptr, size_t size) {
    (void)opaque;
    if (!size) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, size);
}
typedef struct {
    Count ticks;
    uintptr_t base;
} RegexBudget;
static uintptr_t stack_cell(uintptr_t pointer) {
#if CHAR_BIT == 16
    return (pointer >> 16) * 64000UL + (pointer & 65535UL);
#else
    return pointer;
#endif
}
QInt lre_check_stack_overflow(void *opaque, size_t size) {
    RegexBudget *budget = opaque;
    unsigned local;
    uintptr_t here = stack_cell((uintptr_t)&local);
    uintptr_t used = here < budget->base ? budget->base - here : here - budget->base;
    return used > 6144UL || size > 6144UL - used;
}
QInt lre_check_timeout(void *opaque) {
    RegexBudget *budget = opaque;
    return budget && ++budget->ticks > 100000;
}
static int variable(Id x) {
    unsigned tag = syma_tag(x);
    return tag == SYMA_T_VAR || tag == SYMA_T_REST;
}
static Id wildcard_index(Id x, const char *prefix, Count *nv, Count *nr, unsigned depth) {
    Seq *a = NULL;
    Id h, y;
    Count i;
    char b[64];
    if (!x)
        return 0;
    if (depth > 256)
        syma_fail("rule nesting limit exceeded");
    if (syma_headis(x, "/") && syma_argcnt(x) == 1)
        return x;
    if (variable(x) && syma_is(syma_arg(x, 0), "_")) {
        Count *n = syma_headis(x, "Var") ? nv : nr;
        sprintf(b, "%s%s%lu", prefix, syma_headis(x, "Var") ? "v" : "r", (unsigned long)(*n)++);
        return syma_call(syma_node(x)->h, syma_one(syma_str(b)));
    }
    if (syma_node(x)->kind != CALL)
        return x;
    h = wildcard_index(syma_node(x)->h, prefix, nv, nr, depth + 1);
    for (i = 0; i < syma_argcnt(x); i++) {
        y = wildcard_index(syma_arg(x, i), prefix, nv, nr, depth + 1);
        a = syma_cat(a, syma_one(y));
    }
    return syma_call(h, a);
}
void syma_rules_init(Rules *r) { memset(r, 0, sizeof(*r)); }
void syma_rules_free(Rules *r) {
    free(r->v);
    free(r->buckets);
    syma_rules_init(r);
}
void syma_rules_add(Rules *rs, Id ast) {
    Rule r;
    Count i = 0, nv = 0, nr = 0, rv = 0, rr = 0;
    Id opt;
    char b[256];
    memset(&r, 0, sizeof(r));
    if (syma_headis(ast, "TaggedRule") || syma_headis(ast, "TaggedRuleRule"))
        ast = syma_arg(ast, 1);
    if (!syma_headis(ast, "R") || syma_argcnt(ast) < 3)
        return;
    r.ast = ast;
    r.name = syma_arg(ast, 0);
    if (syma_is(r.name, "rewritten"))
        i++;
    if (syma_argcnt(ast) < i + 3)
        return;
    r.name = syma_arg(ast, i++);
    r.lhs = syma_arg(ast, i++);
    r.rhs = syma_arg(ast, i++);
    while (i < syma_argcnt(ast)) {
        opt = syma_arg(ast, i++);
        if (syma_is(opt, ":guard") && i < syma_argcnt(ast))
            r.guard = syma_arg(ast, i++);
        else if (syma_is(opt, ":prio") && i < syma_argcnt(ast)) {
            Id v = syma_arg(ast, i++);
            if (syma_node(v)->kind == NUM)
                r.prio = syma_node(v)->num;
        } else if (syma_is(opt, ":scope") && i < syma_argcnt(ast))
            r.scope = syma_arg(ast, i++);
        else if (syma_is(opt, ":with") && i < syma_argcnt(ast))
            r.with = syma_arg(ast, i++);
        else if (syma_is(opt, ":let") && i < syma_argcnt(ast))
            r.let = syma_arg(ast, i++);
        else if (syma_is(opt, ":innermost"))
            r.inner = 1;
    }
    r.lhs = wildcard_index(r.lhs, "$", &nv, &nr, 0);
    r.rhs = wildcard_index(r.rhs, "$", &rv, &rr, 0);
    if ((rv && rv != nv) || (rr && rr != nr)) {
        char *name = syma_utf8(r.name);
        sprintf(b, "Rule \"%.100s\": wildcard count mismatch", name);
        free(name);
        syma_fail(b);
    }
    nv = nr = 0;
    r.guard = wildcard_index(r.guard, "$g", &nv, &nr, 0);
    nv = nr = 0;
    r.with = wildcard_index(r.with, "$w", &nv, &nr, 0);
    nv = nr = 0;
    r.let = wildcard_index(r.let, "$l", &nv, &nr, 0);
    if (variable(r.lhs))
        rs->atom_types |= 112;
    else if (syma_node(r.lhs)->kind != CALL)
        rs->atom_types |= syma_node(r.lhs)->flags & 112;
    r.order = rs->n;
    r.universal = 1;
    if (!variable(r.lhs)) {
        Node *p = syma_node(r.lhs);
        if (p->kind != CALL) {
            r.key = r.lhs;
            r.universal = 0;
        } else if (!variable(p->h) && p->h && syma_node(p->h)->kind == SYM) {
            r.key = p->h;
            r.universal = 0;
        }
    }
    if (rs->n == rs->cap) {
        rs->cap = rs->cap ? rs->cap * 2 : 16;
        rs->v = syma_resize(rs->v, rs->cap * sizeof(Rule));
    }
    rs->v[rs->n++] = r;
    rs->hasinner |= r.inner;
}
void syma_rules_index(Rules *r) {
    Count i, j;
    Rule v;
    free(r->buckets);
    r->bucketn = 1021;
    r->buckets = syma_mem(r->bucketn * sizeof(Count));
    for (i = 1; i < r->n; i++) {
        v = r->v[i];
        j = i;
        while (j && r->v[j - 1].prio < v.prio) {
            r->v[j] = r->v[j - 1];
            j--;
        }
        r->v[j] = v;
    }
    for (i = r->n; i > 0; i--) {
        Rule *p = &r->v[i - 1];
        Count b = p->universal ? 0 : 1 + syma_node(p->key)->hash % (r->bucketn - 1);
        p->next = r->buckets[b];
        r->buckets[b] = i;
    }
}
Id syma_section(Id x, const char *name) {
    Count i;
    for (i = 0; i < syma_argcnt(x); i++)
        if (syma_headis(syma_arg(x, i), name))
            return syma_arg(x, i);
    return 0;
}
Id syma_replace_section(Id x, const char *name, Id replacement) {
    Count i;
    Seq *a = NULL;
    unsigned found = 0;
    for (i = 0; i < syma_argcnt(x); i++) {
        Id v = syma_arg(x, i);
        if (syma_headis(v, name)) {
            v = replacement;
            found = 1;
        }
        a = syma_cat(a, syma_one(v));
    }
    if (!found)
        a = syma_cat(a, syma_one(replacement));
    return syma_call(syma_node(x)->h, a);
}
void syma_rules_extract(Rules *r, Id universe) {
    Id s = syma_headis(universe, "Rules") ? universe : syma_section(universe, "Rules");
    Count i;
    for (i = 0; i < syma_argcnt(s); i++)
        syma_rules_add(r, syma_arg(s, i));
    syma_rules_index(r);
}
typedef struct {
    Id name, value;
    Seq *seq;
    unsigned rest;
} Binding;
typedef struct Task Task;
struct Task {
    Task *next, *allocated;
    unsigned kind;
    Id p, s;
    Seq *ps, *ts;
};
typedef struct {
    Task *todo;
    Count trail;
    Seq *pattern, *subject;
    Id name;
    Count take, minimum;
    unsigned rest, reverse;
} Choice;
typedef struct TaskPage TaskPage;
struct TaskPage {
    TaskPage *next, *allnext;
    Binding *owned_bindings;
    Choice *owned_choices;
    Count used;
    Task slots[32];
};
static TaskPage *unused_task_pages, *all_task_pages;
typedef struct {
    Binding *b;
    Count n, cap;
    Task *todo, *allocated;
    TaskPage *pages, *owner;
    Choice *choices;
    Count nc, cc, work;
    Count slots[64];
    Binding inline_bindings[8];
    Choice inline_choices[8];
} Match;
static void matchinit(Match *m) {
    memset(m, 0, (Count)offsetof(Match, inline_bindings));
    m->b = m->inline_bindings;
    m->cap = 8;
    m->choices = m->inline_choices;
    m->cc = 8;
}
void syma_match_cleanup(void) {
    TaskPage *p, *next;
    for (p = all_task_pages; p; p = next) {
        next = p->allnext;
        free(p->owned_bindings);
        free(p->owned_choices);
        free(p);
    }
    unused_task_pages = all_task_pages = NULL;
}

static void task(Match *m, unsigned kind, Id p, Id s, Seq *ps, Seq *ts) {
    Task *t;
    if (!m->pages || m->pages->used == 32) {
        TaskPage *page;
        if (unused_task_pages) {
            page = unused_task_pages;
            unused_task_pages = page->next;
        } else {
            page = malloc(sizeof(TaskPage));
            if (!page)
                syma_fail("out of pattern memory");
            page->allnext = all_task_pages;
            all_task_pages = page;
        }
        page->owned_bindings = NULL;
        page->owned_choices = NULL;
        page->used = 0;
        page->next = m->pages;
        m->pages = page;
        if (!m->owner)
            m->owner = page;
    }
    t = &m->pages->slots[m->pages->used++];
    t->next = m->todo;
    t->kind = kind;
    t->p = p;
    t->s = s;
    t->ps = ps;
    t->ts = ts;
    m->todo = t;
}
static Binding *binding(Match *m, Id name) {
    Count bucket = syma_node(name)->hash % 64, i = m->slots[bucket];
    if (i && i <= m->n && m->b[i - 1].name == name)
        return &m->b[i - 1];
    for (i = 0; i < m->n; i++)
        if (m->b[i].name == name) {
            m->slots[bucket] = i + 1;
            return &m->b[i];
        }
    return NULL;
}
static int bindeq(Seq *a, Seq *b) {
    Count i;
    if (a == b)
        return 1;
    if (syma_length(a) != syma_length(b))
        return 0;
    for (i = 0; i < syma_length(a); i++)
        if (syma_at(a, i) != syma_at(b, i))
            return 0;
    return 1;
}
static int bind(Match *m, Id name, Id value, Seq *seq, unsigned rest) {
    Binding *b;
    if (syma_is(name, "_"))
        return 1;
    b = binding(m, name);
    if (b)
        return b->rest == rest && (rest ? bindeq(b->seq, seq) : b->value == value);
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 16;
        if (m->b == m->inline_bindings) {
            Binding *fresh = syma_mem(m->cap * sizeof(Binding));
            memcpy(fresh, m->b, m->n * sizeof(Binding));
            m->b = fresh;
        } else
            m->b = syma_resize(m->b, m->cap * sizeof(Binding));
    }
    if (m->b != m->inline_bindings)
        m->owner->owned_bindings = m->b;
    b = &m->b[m->n++];
    b->name = name;
    b->value = value;
    b->seq = seq;
    b->rest = rest;
    m->slots[syma_node(name)->hash % 64] = m->n;
    return 1;
}
static void choice(Match *m, Task *t, Count trail) {
    if (m->nc == m->cc) {
        m->cc = m->cc ? m->cc * 2 : 16;
        if (m->choices == m->inline_choices) {
            Choice *fresh = syma_mem(m->cc * sizeof(Choice));
            memcpy(fresh, m->choices, m->nc * sizeof(Choice));
            m->choices = fresh;
        } else
            m->choices = syma_resize(m->choices, m->cc * sizeof(Choice));
    }
    if (m->choices != m->inline_choices)
        m->owner->owned_choices = m->choices;
    m->choices[m->nc].todo = t;
    m->choices[m->nc].rest = 0;
    m->choices[m->nc++].trail = trail;
}
static int backtrack(Match *m) {
    Choice c;
    if (!m->nc)
        return 0;
    c = m->choices[--m->nc];
    m->todo = c.todo;
    m->n = c.trail;
    if (c.rest) {
        Count take = c.take;
        if ((c.reverse && take > c.minimum) || (!c.reverse && take < syma_length(c.subject))) {
            choice(m, c.todo, c.trail);
            c.take = c.reverse ? take - 1 : take + 1;
            m->choices[m->nc - 1] = c;
        }
        task(m, 1, 0, 0, c.pattern, syma_slice(c.subject, take, syma_length(c.subject) - take));
        task(m, 2, c.name, 0, NULL, syma_slice(c.subject, 0, take));
    }
    return 1;
}
static void matchfree(Match *m) {
    TaskPage *p, *next;
    for (p = m->pages; p; p = next) {
        next = p->next;
        p->next = unused_task_pages;
        unused_task_pages = p;
    }
    if (m->choices != m->inline_choices)
        free(m->choices);
    if (m->b != m->inline_bindings)
        free(m->b);
    if (m->owner) {
        m->owner->owned_bindings = NULL;
        m->owner->owned_choices = NULL;
    }
}
static int nextmatch(Match *m);
static int quickmatch(Id p, Id s) {
    Match m;
    int ok;
    matchinit(&m);
    task(&m, 0, p, s, NULL, NULL);
    ok = nextmatch(&m);
    matchfree(&m);
    return ok;
}
static int regexseq(Match *m, Id p, Seq *ts, Seq **remaining) {
    Buffer text = {0};
    Count i, n = 0, consumed = 0, total = 0;
    RegexBudget budget;
    budget.ticks = 0;
    budget.base = stack_cell((uintptr_t)&budget);
    char *pattern, *s;
    Octet *bc, **captures;
    QInt size, ncap, ok;
    char error[160];
    Unit *subject;
    Id st;
    Seq *a = NULL;
    Count cut;
    if (!syma_argcnt(p) || syma_node(syma_arg(p, 0))->kind != STR)
        return 0;
    while (n < syma_length(ts) && syma_node(syma_at(ts, n))->kind == STR) {
        total += syma_node(syma_at(ts, n))->len;
        n++;
    }
    if (!n)
        return 0;
    subject = syma_mem((total + 1) * sizeof(Unit));
    for (i = 0; i < n; i++) {
        Node *v = syma_node(syma_at(ts, i));
        memcpy(subject + consumed, v->s, v->len * sizeof(Unit));
        consumed += v->len;
    }
    pattern = syma_utf8(syma_arg(p, 0));
    bc = lre_compile(&size, error, 160, pattern, strlen(pattern), 0, &budget);
    free(pattern);
    if (!bc) {
        free(subject);
        return 0;
    }
    ncap = lre_get_capture_count(bc);
    captures = syma_mem((Count)lre_get_alloc_count(bc) * sizeof(Octet *));
    ok = lre_exec(captures, bc, (Octet *)subject, 0, (QInt)total, 1, &budget);
    if (ok <= 0) {
        free(bc);
        free(captures);
        free(subject);
        if (ok < 0)
            syma_fail("regexp resource limit exceeded");
        return 0;
    }
    consumed = (Count)((Unit *)captures[1] - (Unit *)captures[0]);
    for (i = 1; i < syma_argcnt(p); i++) {
        Id v = syma_arg(p, i);
        if (syma_tag(v) != SYMA_T_VAR)
            continue;
        if (i > (Count)ncap || !captures[2 * (i - 1)]) {
            ok = 0;
            break;
        }
        cut = (Count)((Unit *)captures[2 * (i - 1) + 1] - (Unit *)captures[2 * (i - 1)]);
        st = syma_atom(STR, (Unit *)captures[2 * (i - 1)], cut);
        if (!bind(m, syma_arg(v, 0), st, NULL, 0)) {
            ok = 0;
            break;
        }
    }
    cut = consumed;
    i = 0;
    while (i < n && cut >= syma_node(syma_at(ts, i))->len) {
        cut -= syma_node(syma_at(ts, i))->len;
        i++;
    }
    if (i < n && cut) {
        Node *v = syma_node(syma_at(ts, i));
        a = syma_one(syma_atom(STR, v->s + cut, v->len - cut));
        i++;
    }
    *remaining = syma_cat(a, syma_slice(ts, i, syma_length(ts) - i));
    free(bc);
    free(captures);
    free(subject);
    free(text.p);
    (void)s;
    return ok > 0;
}
static int nextmatch(Match *m) {
    Task *t, *base;
    Id p, s, v;
    Seq *ps, *ts, *remaining;
    Count i, n, save, min;
    Binding *b;
    int ok;
    unsigned ptag;
    for (;;) {
        if (!m->todo)
            return 1;
        if (++m->work > 1000000UL)
            syma_fail("pattern search limit exceeded");
        t = m->todo;
        m->todo = t->next;
        p = t->p;
        s = t->s;
        if (t->kind == 3) {
            m->nc = p;
            continue;
        }
        if (t->kind == 4) {
            task(m, 3, m->nc, 0, NULL, NULL);
            task(m, 0, p, s, NULL, NULL);
            continue;
        }
        if (t->kind == 2) {
            if (!bind(m, p, 0, t->ts, 1))
                goto bad;
            continue;
        }
        if (t->kind == 1) {
            ps = t->ps;
            ts = t->ts;
            if (!ps) {
                if (ts)
                    goto bad;
                continue;
            }
            p = syma_at(ps, 0);
            remaining = syma_slice(ps, 1, syma_length(ps) - 1);
            if (syma_node(p)->kind != CALL) {
                if (!ts || p != syma_at(ts, 0))
                    goto bad;
                task(m, 1, 0, 0, remaining, syma_slice(ts, 1, syma_length(ts) - 1));
                continue;
            }
            if (syma_tag(p) == SYMA_T_VAR) {
                if (!ts || !bind(m, syma_arg(p, 0), syma_at(ts, 0), NULL, 0))
                    goto bad;
                task(m, 1, 0, 0, remaining, syma_slice(ts, 1, syma_length(ts) - 1));
                continue;
            }
            ptag = syma_tag(p);
            min = 0;
            v = p;
            if (ptag == SYMA_T_REPEAT && syma_argcnt(p) == 1 &&
                syma_headis(syma_arg(p, 0), "VarRest")) {
                v = syma_arg(p, 0);
                min = 1;
            }
            if (syma_tag(v) == SYMA_T_REST) {
                if (syma_length(ts) < min)
                    goto bad;
                if (!remaining) {
                    if (!bind(m, syma_arg(v, 0), 0, ts, 1))
                        goto bad;
                    continue;
                }
                /* Rest splits are shortest first, except an immediately following
                 * greedy anchor. A choice stores only the next split, not an
                 * array of every possible binding environment. */
                choice(m, m->todo, m->n);
                Choice *split = &m->choices[m->nc - 1];
                split->rest = 1;
                split->pattern = remaining;
                split->subject = ts;
                split->name = syma_arg(v, 0);
                split->minimum = min;
                split->reverse = remaining && syma_headis(syma_at(remaining, 0), "GreedyAnchor");
                split->take = split->reverse ? syma_length(ts) : min;
                if (!backtrack(m))
                    goto bad;
                continue;
            }
            if (ptag == SYMA_T_REGEX) {
                save = m->n;
                if (!regexseq(m, p, ts, &ts)) {
                    m->n = save;
                    goto bad;
                }
                task(m, 1, 0, 0, remaining, ts);
                continue;
            }
            if (ptag == SYMA_T_REPEAT && syma_argcnt(p) >= 1) {
                Id atompat = syma_arg(p, syma_argcnt(p) == 2 ? 1 : 0);
                n = 0;
                while (n < syma_length(ts) && quickmatch(atompat, syma_at(ts, n)))
                    n++;
                if (!n)
                    goto bad;
                if (syma_argcnt(p) == 2 && syma_headis(syma_arg(p, 0), "VarRest") &&
                    !bind(m, syma_arg(syma_arg(p, 0), 0), 0, syma_slice(ts, 0, n), 1))
                    goto bad;
                task(m, 1, 0, 0, remaining, syma_slice(ts, n, syma_length(ts) - n));
                continue;
            }
            if (ptag == SYMA_T_META_MATCH && syma_argcnt(p) == 1 &&
                syma_headis(syma_arg(p, 0), "VarRest")) {
                b = binding(m, syma_arg(syma_arg(p, 0), 0));
                if (syma_is(syma_arg(syma_arg(p, 0), 0), "_")) {
                    task(m, 1, 0, 0, remaining, ts);
                    continue;
                }
                if (!b || !b->rest || syma_length(ts) < syma_length(b->seq))
                    goto bad;
                n = syma_length(b->seq);
                task(m, 1, 0, 0, remaining, syma_slice(ts, n, syma_length(ts) - n));
                task(m, 1, 0, 0, b->seq, syma_slice(ts, 0, n));
                continue;
            }
            if (!ts)
                goto bad;
            task(m, 1, 0, 0, remaining, syma_slice(ts, 1, syma_length(ts) - 1));
            task(m, 0, p, syma_at(ts, 0), NULL, NULL);
            continue;
        }
        if (p && syma_node(p)->kind != CALL) {
            if (p != s)
                goto bad;
            continue;
        }
        ptag = syma_tag(p);
        if (ptag == SYMA_T_VAR) {
            if (!bind(m, syma_arg(p, 0), s, NULL, 0))
                goto bad;
            continue;
        }
        if (ptag == SYMA_T_GREEDY) {
            task(m, 0, syma_arg(p, 0), s, NULL, NULL);
            continue;
        }
        if (ptag == SYMA_T_META_MATCH && syma_argcnt(p) == 1) {
            v = syma_arg(p, 0);
            if (syma_tag(v) == SYMA_T_VAR) {
                if (syma_is(syma_arg(v, 0), "_"))
                    continue;
                b = binding(m, syma_arg(v, 0));
                if (!b || b->rest)
                    goto bad;
                v = b->value;
            }
            task(m, 0, v, s, NULL, NULL);
            continue;
        }
        if (ptag == SYMA_T_ALT || ptag == SYMA_T_NOT) {
            save = m->n;
            i = 0;
            if (syma_argcnt(p) >= 2 && syma_headis(syma_arg(p, 0), "Var")) {
                if (!bind(m, syma_arg(syma_arg(p, 0), 0), s, NULL, 0))
                    goto bad;
                i = 1;
            }
            if (ptag == SYMA_T_NOT) {
                ok = 1;
                for (; i < syma_argcnt(p); i++)
                    if (quickmatch(syma_arg(p, i), s)) {
                        ok = 0;
                        break;
                    }
                if (!ok) {
                    m->n = save;
                    goto bad;
                }
                continue;
            }
            base = m->todo;
            n = syma_argcnt(p);
            save = m->n;
            while (n > i) {
                n--;
                m->todo = base;
                task(m, 0, syma_arg(p, n), s, NULL, NULL);
                choice(m, m->todo, save);
            }
            m->todo = base;
            if (!backtrack(m))
                goto bad;
            continue;
        }
        if (!p || !s || syma_node(p)->kind != CALL) {
            if (p != s)
                goto bad;
            continue;
        }
        if (syma_node(s)->kind != CALL)
            goto bad;
        if (!syma_node(p)->h && !syma_node(s)->h) {
            task(m, 1, 0, 0, syma_node(p)->a, syma_node(s)->a);
            continue;
        }
        if (!syma_node(p)->h || (!syma_node(s)->h && !syma_headis(syma_node(p)->h, "VarRest")))
            goto bad;
        Id ph = syma_node(p)->h;
        if (ph && syma_node(ph)->kind != CALL) {
            if (ph != syma_node(s)->h)
                goto bad;
            task(m, 1, 0, 0, syma_node(p)->a, syma_node(s)->a);
        } else
            task(m, 1, 0, 0, syma_flat(p), syma_flat(s));
        continue;
    bad:
        if (!backtrack(m))
            return 0;
    }
}
static Id splice(Seq *s) { /* Internal splices are never interned as public terms. */
    return syma_named("$splice", s);
}
static Id substitute(Id x, Match *m, unsigned preserve, unsigned depth) {
    Node *p = syma_node(x);
    Binding *b;
    Seq *a = NULL, *f;
    Id h, y;
    Count i;
    if (depth > 256)
        syma_fail("substitution nesting limit exceeded");
    if (!p)
        return 0;
    if (!(p->flags & (1 | 8)))
        return x;
    unsigned xtag = syma_tag(x);
    if (xtag == SYMA_T_QUOTE) {
        if (syma_argcnt(x) == 1)
            return syma_arg(x, 0);
        return splice(p->a);
    }
    if (xtag == SYMA_T_UNBOUND && syma_argcnt(x) == 1 && syma_node(syma_arg(x, 0))->kind == STR) {
        Node *v = syma_node(syma_arg(x, 0));
        if (v->len >= 3 && v->s[v->len - 1] == '.' && v->s[v->len - 2] == '.' &&
            v->s[v->len - 3] == '.')
            return syma_named("VarRest", syma_one(syma_atom(STR, v->s, v->len - 3)));
        return syma_named("Var", syma_one(syma_arg(x, 0)));
    }
    if (xtag == SYMA_T_VAR || xtag == SYMA_T_REST) {
        b = binding(m, syma_arg(x, 0));
        if (!b) {
            if (preserve)
                return x;
            if (xtag == SYMA_T_REST)
                return splice(NULL);
            syma_fail("subst: unbound variable");
        }
        if (xtag == SYMA_T_REST) {
            if (!b->rest)
                syma_fail("subst: rest variable expected sequence");
            if (!b->seq || !(b->seq->flags & (1 | 8)))
                return splice(b->seq);
            for (i = 0; i < syma_length(b->seq); i++)
                a = syma_cat(a, syma_one(substitute(syma_at(b->seq, i), m, preserve, depth + 1)));
            return splice(a);
        }
        if (b->rest)
            syma_fail("subst: scalar variable expected term");
        return b->value;
    }
    if (p->kind != CALL || !(p->flags & (1 | 8)))
        return x;
    if (syma_tag(x) == SYMA_T_META_SAFE)
        preserve = 1;
    h = substitute(p->h, m, preserve, depth + 1);
    if (syma_tag(h) == SYMA_T_SPLICE)
        a = syma_node(h)->a;
    else if (h)
        a = syma_one(h);
    for (i = 0; i < syma_argcnt(x); i++) {
        y = substitute(syma_arg(x, i), m, preserve, depth + 1);
        f = syma_tag(y) == SYMA_T_SPLICE ? syma_node(y)->a : syma_one(y);
        a = syma_cat(a, f);
    }
    if (!p->h)
        return syma_call(0, a);
    return syma_fromflat(a);
}
/* REPL inspection shares the runtime matcher and substitution semantics. */
int syma_match_term(Id pattern, Id subject, Id replacement, Id *bindings, Id *result) {
    Match m;
    Count i;
    Seq *items = NULL;
    int ok;
    matchinit(&m);
    task(&m, 0, pattern, subject, NULL, NULL);
    ok = nextmatch(&m);
    if (ok) {
        for (i = 0; i < m.n; i++) {
            Binding *b = &m.b[i];
            items = syma_cat(items,
                             syma_one(syma_named(
                                 b->rest ? "Rest" : "Binding",
                                 syma_cat(syma_one(b->name),
                                          syma_one(b->rest ? syma_call(0, b->seq) : b->value)))));
        }
        if (bindings)
            *bindings = syma_named("Bindings", items);
        if (result)
            *result = replacement ? substitute(replacement, &m, 0, 0) : subject;
    }
    matchfree(&m);
    return ok;
}
typedef struct {
    Id x;
    Count parent, index;
    unsigned meta;
} Visit;
typedef struct VisitPool VisitPool;
struct VisitPool {
    VisitPool *next, *allnext;
    Visit *items;
    Count capacity;
    Count *order;
    Count ordercap;
};
static VisitPool *visit_pool, *all_visits;
static VisitPool *take_visits(void) {
    VisitPool *p;
    if (visit_pool) {
        p = visit_pool;
        visit_pool = p->next;
    } else {
        p = syma_mem(sizeof(*p));
        p->allnext = all_visits;
        all_visits = p;
    }
    return p;
}
static void put_visits(VisitPool *p) {
    p->next = visit_pool;
    visit_pool = p;
}
void syma_visit_cleanup(void) {
    VisitPool *p, *next;
    for (p = all_visits; p; p = next) {
        next = p->allnext;
        free(p->items);
        free(p->order);
        free(p);
    }
    all_visits = visit_pool = NULL;
}
static Id tryrule(Engine *e, Rule *r, Id x, Visit *vis, Count vi, unsigned meta,
                  unsigned *matched) {
    Match m;
    Id context = x, g, y;
    Count k, save;
    Engine guard;
    if (r->scope) {
        unsigned found = 0;
        char *scope_name = syma_utf8(r->scope);
        Count *anc = syma_mem((vi + 1) * sizeof(Count)), n = 0;
        k = vis[vi].parent;
        while (k != (Count)-1) {
            anc[n++] = k;
            k = vis[k].parent;
        }
        while (n) {
            k = anc[--n];
            if (syma_node(vis[k].x)->kind == CALL && syma_node(vis[k].x)->h &&
                syma_node(syma_node(vis[k].x)->h)->kind == SYM &&
                syma_is(syma_node(vis[k].x)->h, scope_name)) {
                context = vis[k].x;
                found = 1;
                break;
            }
        }
        free(anc);
        free(scope_name);
        if (!found)
            return 0;
    } else if ((r->with || r->let) && e->universe) {
        Id program = syma_section(e->universe, "Program");
        context = program ? program : vis[0].x;
        if (syma_headis(vis[0].x, "Program"))
            context = vis[0].x;
    } else if (r->with || r->let)
        context = vis[0].x;
    matchinit(&m);
    if (r->with)
        task(&m, 4, r->with, context, NULL, NULL);
    task(&m, 0, r->lhs, x, NULL, NULL);
    if (r->let)
        task(&m, 4, r->let, context, NULL, NULL);
    while (nextmatch(&m)) {
        if (r->guard) {
            g = substitute(r->guard, &m, e->preserve || meta, 0);
            guard = *e;
            guard.maxsteps = 100;
            guard.depth = e->depth + 1;
            guard.trace = 0;
            guard.trace_counts = NULL;
            guard.meta = 0;
            guard.excluded = (Count)(r - e->rules.v) + 1;
            if (guard.excluded_count == 32)
                syma_fail("nested guard limit exceeded");
            guard.excluded_rules[guard.excluded_count++] = guard.excluded;
            guard.universe = e->universe;
            g = syma_normalize(&guard, g);
            if (!syma_is(g, "True")) {
                if (!backtrack(&m))
                    break;
                continue;
            }
        }
        y = substitute(r->rhs, &m, e->preserve || meta, 0);
        *matched = 1;
        matchfree(&m);
        return y;
    }
    save = m.n;
    (void)save;
    matchfree(&m);
    return 0;
}
int syma_probe_rule(Engine *e, Count index, Id subject, Id *result) {
    Visit root;
    unsigned matched = 0;
    root.x = subject;
    root.parent = (Count)-1;
    root.index = 0;
    root.meta = e->meta;
    *result = tryrule(e, &e->rules.v[index], subject, &root, 0, e->meta, &matched);
    return matched;
}
static Id rewrite(Engine *e, Id root, unsigned inner, unsigned *changed) {
    VisitPool *pool = take_visits();
    Visit *v = pool->items;
    Count n = 0, cap = pool->capacity, pos, k, i, cand, univ, bucket, walk;
    Count *order = NULL, *first = NULL, *next = NULL;
    Id x, key, y;
    unsigned meta;
    Rule *r;
#define PUSH(X, P, I, M)                                                                           \
    do {                                                                                           \
        if (n == cap) {                                                                            \
            cap = cap ? cap * 2 : 64;                                                              \
            v = syma_resize(v, cap * sizeof(Visit));                                               \
            pool->items = v;                                                                       \
            pool->capacity = cap;                                                                  \
        }                                                                                          \
        v[n].x = (X);                                                                              \
        v[n].parent = (P);                                                                         \
        v[n].index = (I);                                                                          \
        v[n].meta = (M);                                                                           \
        n++;                                                                                       \
    } while (0)
    PUSH(root, (Count)-1, 0, e->meta);
    for (pos = 0; pos < n; pos++) {
        x = v[pos].x;
        if (!x || syma_node(x)->kind != CALL || syma_headis(x, "Inert"))
            continue;
        meta = v[pos].meta || syma_tag(x) == SYMA_T_META_SAFE;
        Id childhead = syma_node(x)->h;
        unsigned eligible = e->rules.atom_types | 2;
        if (childhead && (syma_node(childhead)->flags & eligible))
            PUSH(childhead, pos, (Count)-1, meta);
        if (syma_node(x)->a && (syma_node(x)->a->flags & eligible))
            for (i = 0; i < syma_argcnt(x); i++) {
                Id child = syma_arg(x, i);
                if (syma_node(child)->flags & eligible)
                    PUSH(child, pos, i, meta);
            }
    }
    if (inner) {
        if (pool->ordercap < n) {
            pool->order = syma_resize(pool->order, n * 3 * sizeof(Count));
            pool->ordercap = n;
        }
        order = pool->order;
        first = order + n;
        next = first + n;
        for (i = 0; i < n; i++)
            first[i] = next[i] = (Count)-1;
        for (i = n; i > 1; i--) {
            k = i - 1;
            next[k] = first[v[k].parent];
            first[v[k].parent] = k;
        }
        k = 0;
        i = 0;
        for (;;) {
            if (first[k] != (Count)-1) {
                Count child = first[k];
                first[k] = (Count)-1;
                k = child;
                continue;
            }
            order[i++] = k;
            if (next[k] != (Count)-1)
                k = next[k];
            else if (v[k].parent != (Count)-1)
                k = v[k].parent;
            else
                break;
        }
    }
    for (walk = 0; walk < n; walk++) {
        pos = inner ? order[walk] : walk;
        x = v[pos].x;
        if (!x || syma_headis(x, "Inert")) {
            continue;
        }
        key = syma_node(x)->kind == CALL ? syma_node(x)->h : x;
        bucket = key ? 1 + syma_node(key)->hash % (e->rules.bucketn - 1) : 1;
        cand = e->rules.buckets[bucket];
        univ = e->rules.buckets[0];
        while (cand || univ) {
            if (!cand || (univ && univ < cand)) {
                k = univ - 1;
                univ = e->rules.v[k].next;
            } else {
                k = cand - 1;
                cand = e->rules.v[k].next;
            }
            r = &e->rules.v[k];
            if (r->inner != inner || e->excluded == k + 1 || (!r->universal && r->key != key))
                continue;
            Count exclusion;
            for (exclusion = 0; exclusion < e->excluded_count; exclusion++)
                if (e->excluded_rules[exclusion] == k + 1)
                    break;
            if (exclusion < e->excluded_count)
                continue;
            y = tryrule(e, r, x, v, pos, v[pos].meta, changed);
            if (*changed) {
                e->rewrites++;
                if (e->trace_counts)
                    e->trace_counts[k]++;
                if (e->trace && !(e->trace & 8)) {
                    char *name = syma_utf8(r->name), *text = syma_show(y, 0);
                    fprintf(stderr, "[%lu] %s: %s\n", (unsigned long)e->steps, name, text);
                    if (e->trace & (2 | 4)) {
                        char *before = syma_show(x, 0);
                        fprintf(stderr, "  before: %s\n  after:  %s\n", before, text);
                        free(before);
                    }
                    if (e->trace & 2) {
                        char *pattern = syma_show(r->lhs, 0), *rhs = syma_show(r->rhs, 0);
                        fprintf(stderr, "  pattern: %s\n  replacement: %s\n  path:", pattern, rhs);
                        Count path = pos;
                        while (v[path].parent != (Count)-1) {
                            fprintf(stderr, " %ld", (long)v[path].index);
                            path = v[path].parent;
                        }
                        fprintf(stderr, " (leaf to root)\n");
                        if (r->guard) {
                            char *guard = syma_show(r->guard, 0);
                            fprintf(stderr, "  guard: %s\n", guard);
                            free(guard);
                        }
                        free(pattern);
                        free(rhs);
                    }
                    free(name);
                    free(text);
                }
                k = pos;
                while (v[k].parent != (Count)-1) {
                    Count parent = v[k].parent;
                    Node *p = syma_node(v[parent].x);
                    if (v[k].index == (Count)-1)
                        y = syma_call(y, p->a);
                    else
                        y = syma_call(
                            p->h, syma_cat(syma_cat(syma_slice(p->a, 0, v[k].index), syma_one(y)),
                                           syma_slice(p->a, v[k].index + 1,
                                                      syma_length(p->a) - v[k].index - 1)));
                    k = parent;
                }
                put_visits(pool);
                return y;
            }
        }
    }
    put_visits(pool);
    return root;
#undef PUSH
}
Id syma_normalize(Engine *e, Id x) {
    Count i, allocated = syma_allocated_sequences();
    Id y, z;
    unsigned changed;
    unsigned progress = !e->depth && getenv("SYMA_TEST_PROGRESS") != NULL;
    if (e->depth > 32)
        syma_fail("nested normalization limit exceeded");
    if (!e->rules.buckets)
        syma_rules_index(&e->rules);
    for (i = 0; i < e->maxsteps; i++) {
        e->steps = i;
        if (progress && !(i % 1000)) {
            printf("SYMA step=%lu\n", (unsigned long)i);
            fflush(stdout);
        }
        changed = 0;
        y = x;
        if (e->rules.hasinner)
            y = rewrite(e, x, 1, &changed);
        if (!changed)
            y = rewrite(e, x, 0, &changed);
        z = e->skipprims ? y : syma_fold(e, y, e->meta);
        if (z != y)
            e->primitive_steps++;
        if (!changed && z == x) {
            e->steps = i;
            return z;
        }
        x = z;
        if (!e->depth && syma_allocated_sequences() - allocated > 1500) {
            syma_gc(x, e->universe, &e->rules, &e->keep, 1);
            allocated = syma_allocated_sequences();
        }
    }
    syma_fail("normalize: exceeded maxSteps (possible non-termination)");
    return x;
}
