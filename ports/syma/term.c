#define SYMA_TERM_IMPLEMENTATION 1
#include "syma.h"
#include "dtoa.h"
#include "primitive-ids.h"
#define PAGE 256UL
#define MAXPAGES 4096UL
#define HASHN 32749UL
Node *syma_pages[MAXPAGES];
#define pages syma_pages
static Id hashes[HASHN], nextid, freelist;
static uint32_t hash_epochs[HASHN], hash_epoch;
static Id bucket_head(Count bucket) {
    return hash_epochs[bucket] == hash_epoch ? hashes[bucket] : 0;
}
static void next_hash_epoch(void) {
    if (!++hash_epoch) {
        memset(hash_epochs, 0, sizeof(hash_epochs));
        hash_epoch = 1;
    }
}
static Count live, seqcount;
static Seq *sequences, *free_sequences;
typedef struct SeqPage SeqPage;
struct SeqPage {
    SeqPage *next;
    Count used;
    Seq slots[128];
};
static SeqPage *sequence_pages;
static Seq *new_sequence(void) {
    Seq *s;
    if (free_sequences) {
        s = free_sequences;
        free_sequences = s->next;
        memset(s, 0, sizeof(*s));
    } else {
        if (!sequence_pages || sequence_pages->used == 128) {
            SeqPage *page = syma_mem(sizeof(SeqPage));
            page->next = sequence_pages;
            sequence_pages = page;
        }
        s = &sequence_pages->slots[sequence_pages->used++];
    }
    s->next = sequences;
    sequences = s;
    seqcount++;
    return s;
}

jmp_buf syma_error;
jmp_buf *syma_error_target = &syma_error;
char syma_message[256];
void syma_fail(const char *m) {
    strncpy(syma_message, m, 255);
    syma_message[255] = 0;
    longjmp(*syma_error_target, 1);
}
void *syma_mem(Count n) {
    void *p = calloc(1, n ? n : 1);
    if (!p)
        syma_fail("out of memory");
    return p;
}
void *syma_resize(void *p, Count n) {
    p = realloc(p, n ? n : 1);
    if (!p)
        syma_fail("out of memory");
    return p;
}
Node *syma_node(Id i) { return i ? &pages[i / PAGE][i % PAGE] : NULL; }
unsigned syma_tag(Id id) {
    Node *p, *h;
    if (!id)
        return 0;
    p = &pages[id >> 8][id & 255UL];
    if (p->kind != CALL || !p->h)
        return 0;
    h = &pages[p->h >> 8][p->h & 255UL];
    if (h->kind != SYM)
        return 0;
    if (h->primitive == SYMA_P_SLASH)
        return SYMA_T_QUOTE;
    if (h->primitive < 256)
        return 0;
    unsigned tag = h->primitive - 256;
    if (tag == SYMA_T_VAR || tag == SYMA_T_REST) {
        if (!p->a || p->a->n != 1)
            return 0;
        Id name = p->a->items[0];
        if (pages[name >> 8][name & 255UL].kind != STR)
            return 0;
    }
    if (tag == SYMA_T_GREEDY && (!p->a || p->a->n != 1))
        return 0;
    if (tag == SYMA_T_REGEX) {
        if (!p->a)
            return 0;
        Seq *first = p->a;
        while (!first->used)
            first = first->l;
        Id pattern = first->items[0];
        if (pages[pattern >> 8][pattern & 255UL].kind != STR)
            return 0;
    }
    return tag;
}
static uint32_t mix(uint32_t h, uint32_t x) { return (h ^ x) * 16777619UL; }
Count syma_length(Seq *s) { return s ? s->n : 0; }
static unsigned height(Seq *s) { return s ? s->height : 0; }
static Seq *branch(Seq *a, Seq *b) {
    Seq *s;
    if (!a)
        return b;
    if (!b)
        return a;
    s = new_sequence();
    s->l = a;
    s->r = b;
    s->n = a->n + b->n;
    s->height = 1 + (height(a) > height(b) ? height(a) : height(b));
    s->hash = a->hash * b->power + b->hash;
    s->power = a->power * b->power;
    s->flags = a->flags | b->flags;
    return s;
}
Seq *syma_one(Id x) {
    Seq *s = new_sequence();
    s->n = s->used = s->height = 1;
    s->items[0] = x;
    s->hash = syma_node(x)->hash;
    s->power = 16777619UL;
    s->flags = syma_node(x)->flags;
    return s;
}
Seq *syma_cat(Seq *a, Seq *b) {
    if (!a)
        return b;
    if (!b)
        return a;
    if (a->n + b->n <= 8) {
        Seq *s = new_sequence();
        Count i;
        s->n = s->used = a->n + b->n;
        s->height = 1;
        s->power = 1;
        for (i = 0; i < s->n; i++) {
            s->items[i] = i < a->n ? syma_at(a, i) : syma_at(b, i - a->n);
            s->hash = s->hash * 16777619UL + syma_node(s->items[i])->hash;
            s->power *= 16777619UL;
            s->flags |= syma_node(s->items[i])->flags;
        }
        return s;
    }
    if (height(a) > height(b) + 1) {
        Seq *r = syma_cat(a->r, b);
        if (height(r) > height(a->l) + 1)
            return branch(branch(a->l, r->l), r->r);
        return branch(a->l, r);
    }
    if (height(b) > height(a) + 1) {
        Seq *l = syma_cat(a, b->l);
        if (height(l) > height(b->r) + 1)
            return branch(l->l, branch(l->r, b->r));
        return branch(l, b->r);
    }
    return branch(a, b);
}
Id syma_at(Seq *s, Count i) {
    if (!s || i >= s->n)
        return 0;
    while (!s->used) {
        if (i < s->l->n)
            s = s->l;
        else {
            i -= s->l->n;
            s = s->r;
        }
    }
    return s->items[i];
}
Seq *syma_slice(Seq *s, Count start, Count n) {
    Count left;
    if (!n || !s || start >= s->n)
        return NULL;
    if (n > s->n - start)
        n = s->n - start;
    if (!start && n == s->n)
        return s;
    if (s->used) {
        Seq *r = NULL;
        Count i;
        for (i = 0; i < n; i++)
            r = syma_cat(r, syma_one(s->items[start + i]));
        return r;
    }
    left = s->l->n;
    if (start >= left)
        return syma_slice(s->r, start - left, n);
    if (n <= left - start)
        return syma_slice(s->l, start, n);
    return syma_cat(syma_slice(s->l, start, left - start), syma_slice(s->r, 0, n - (left - start)));
}
static int seqeq(Seq *a, Seq *b) {
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
static Id intern(Node *v) {
    Id i;
    Node *p;
    Count bucket = v->hash % HASHN;
    for (i = bucket_head(bucket); i; i = syma_node(i)->link) {
        p = syma_node(i);
        if (p->hash != v->hash || p->kind != v->kind)
            continue;
        if (v->kind == NUM) {
            if (p->num == v->num)
                return i;
        } else if (v->kind == CALL || v->kind == SPLICE) {
            if (p->h == v->h && seqeq(p->a, v->a))
                return i;
        } else if (p->len == v->len && !memcmp(p->s, v->s, v->len * sizeof(Unit))) {
            free(v->s);
            return i;
        }
    }
    if (freelist) {
        i = freelist;
        freelist = syma_node(i)->link;
    } else {
        i = nextid++;
        if (i / PAGE >= MAXPAGES)
            syma_fail("term limit exceeded");
        if (!pages[i / PAGE])
            pages[i / PAGE] = syma_mem(PAGE * sizeof(Node));
    }
    *syma_node(i) = *v;
    syma_node(i)->link = bucket_head(bucket);
    hashes[bucket] = i;
    hash_epochs[bucket] = hash_epoch;
    live++;
    return i;
}
Id syma_atom(unsigned kind, const Unit *s, Count n) {
    Node v;
    Count i;
    memset(&v, 0, sizeof(v));
    v.kind = kind;
    v.flags = kind == SYM ? 16 : kind == STR ? 64 : 0;
    v.len = n;
    v.hash = mix(2166136261UL, kind);
    for (i = 0; i < n; i++)
        v.hash = mix(v.hash, s[i]);
    {
        Id id;
        for (id = bucket_head(v.hash % HASHN); id; id = syma_node(id)->link) {
            Node *p = syma_node(id);
            if (p->kind == kind && p->hash == v.hash && p->len == n &&
                !memcmp(p->s, s, n * sizeof(Unit)))
                return id;
        }
    }
    if (kind == SYM)
        v.primitive = syma_primitive_id(s, n);
    v.s = syma_mem((n + 1) * sizeof(Unit));
    memcpy(v.s, s, n * sizeof(Unit));
    return intern(&v);
}
Id syma_number(double n) {
    Node v;
    unsigned char *p;
    Count i;
    memset(&v, 0, sizeof(v));
    v.kind = NUM;
    v.flags = 32;
    v.num = n == 0 ? 0 : n;
    v.hash = NUM;
    p = (unsigned char *)&v.num;
    for (i = 0; i < sizeof(double); i++)
        v.hash = mix(v.hash, p[i]);
    return intern(&v);
}
Id syma_call(Id h, Seq *a) {
    Node v;
    memset(&v, 0, sizeof(v));
    v.kind = CALL;
    v.h = h;
    v.a = a;
    v.flags = 2 | (h ? syma_node(h)->flags : 0) | (a ? a->flags : 0);
    unsigned tag = h && syma_node(h)->kind == SYM ? syma_node(h)->primitive : 0;
    if (tag == 256 + SYMA_T_VAR || tag == 256 + SYMA_T_REST || tag == 256 + SYMA_T_UNBOUND)
        v.flags |= 1;
    if (tag == SYMA_P_SLASH || tag == 256 + SYMA_T_QUOTE || tag == 256 + SYMA_T_UNBOUND)
        v.flags |= 8;
    v.hash = mix(CALL, h ? syma_node(h)->hash : 0);
    if (a)
        v.hash = v.hash * a->power + a->hash;
    return intern(&v);
}
Id syma_named(const char *h, Seq *a) { return syma_call(syma_sym(h), a); }
Id syma_utf8atom(unsigned kind, const char *s, Count n) {
    Unit local[128], *u;
    Count ai;
    uint32_t ah = mix(2166136261UL, kind);
    for (ai = 0; ai < n && (unsigned char)s[ai] < 128; ai++)
        ah = mix(ah, (unsigned char)s[ai]);
    if (ai == n) {
        Id id;
        for (id = bucket_head(ah % HASHN); id; id = syma_node(id)->link) {
            Node *p = syma_node(id);
            if (p->kind != kind || p->hash != ah || p->len != n)
                continue;
            for (ai = 0; ai < n && p->s[ai] == (unsigned char)s[ai]; ai++) {
            }
            if (ai == n)
                return id;
        }
    }
    u = n < 128 ? local : syma_mem((n + 1) * sizeof(Unit));
    Count i = 0, j = 0;
    uint32_t c, d;
    Id r;
    while (i < n) {
        c = (unsigned char)s[i++];
        if (c >= 0xc0) {
            unsigned k = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
            c &= k == 1 ? 31 : k == 2 ? 15 : 7;
            while (k--) {
                if (i >= n) {
                    c = 0xfffd;
                    break;
                }
                d = (unsigned char)s[i++];
                if ((d & 192) != 128) {
                    c = 0xfffd;
                    break;
                }
                c = (c << 6) | (d & 63);
            }
        }
        if (c > 0xffff && c <= 0x10ffff) {
            c -= 0x10000;
            u[j++] = (Unit)(0xd800 + (c >> 10));
            u[j++] = (Unit)(0xdc00 + (c & 1023));
        } else
            u[j++] = (Unit)c;
    }
    r = syma_atom(kind, u, j);
    if (u != local)
        free(u);
    return r;
}
Id syma_sym(const char *s) { return syma_utf8atom(SYM, s, (Count)strlen(s)); }
Id syma_str(const char *s) { return syma_utf8atom(STR, s, (Count)strlen(s)); }
int syma_is(Id x, const char *s) {
    Node *p = syma_node(x);
    Count i, n = (Count)strlen(s);
    if (!p || (p->kind != SYM && p->kind != STR) || p->len != n)
        return 0;
    for (i = 0; i < n; i++)
        if (p->s[i] != (unsigned char)s[i])
            return 0;
    return 1;
}
int syma_headis(Id x, const char *s) {
    return x && syma_node(x)->kind == CALL && syma_node(x)->h &&
           syma_node(syma_node(x)->h)->kind == SYM && syma_is(syma_node(x)->h, s);
}
Id syma_arg(Id x, Count i) {
    return x && (syma_node(x)->kind == CALL || syma_node(x)->kind == SPLICE)
               ? syma_at(syma_node(x)->a, i)
               : 0;
}
Count syma_argcnt(Id x) {
    return x && (syma_node(x)->kind == CALL || syma_node(x)->kind == SPLICE)
               ? syma_length(syma_node(x)->a)
               : 0;
}
Seq *syma_flat(Id x) {
    Node *p = syma_node(x);
    return p->h ? syma_cat(syma_one(p->h), p->a) : p->a;
}
Id syma_fromflat(Seq *s) {
    return syma_call(syma_at(s, 0), syma_slice(s, 1, syma_length(s) ? syma_length(s) - 1 : 0));
}
void syma_bufput(Buffer *b, unsigned c) {
    if (b->n + 2 >= b->cap) {
        b->cap = b->cap ? b->cap * 2 : 128;
        b->p = syma_resize(b->p, b->cap * sizeof(char));
    }
    b->p[b->n++] = (char)c;
    b->p[b->n] = 0;
}
void syma_buftext(Buffer *b, const char *s) {
    while (*s)
        syma_bufput(b, (unsigned char)*s++);
}
static void emitutf(Buffer *b, uint32_t c) {
    if (c < 128)
        syma_bufput(b, (unsigned)c);
    else if (c < 2048) {
        syma_bufput(b, (unsigned)(192 | (c >> 6)));
        syma_bufput(b, (unsigned)(128 | (c & 63)));
    } else if (c < 65536) {
        syma_bufput(b, (unsigned)(224 | (c >> 12)));
        syma_bufput(b, (unsigned)(128 | ((c >> 6) & 63)));
        syma_bufput(b, (unsigned)(128 | (c & 63)));
    } else {
        syma_bufput(b, (unsigned)(240 | (c >> 18)));
        syma_bufput(b, (unsigned)(128 | ((c >> 12) & 63)));
        syma_bufput(b, (unsigned)(128 | ((c >> 6) & 63)));
        syma_bufput(b, (unsigned)(128 | (c & 63)));
    }
}
char *syma_utf8(Id x) {
    Buffer b = {0};
    Node *p = syma_node(x);
    Count i;
    uint32_t c;
    if (!p)
        return syma_mem(1);
    for (i = 0; i < p->len; i++) {
        c = p->s[i];
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < p->len && p->s[i + 1] >= 0xdc00 &&
            p->s[i + 1] <= 0xdfff)
            c = 0x10000 + ((c - 0xd800) << 10) + (p->s[++i] - 0xdc00);
        emitutf(&b, c);
    }
    if (!b.p)
        b.p = syma_mem(1);
    return b.p;
}
static void quoted(Buffer *b, Id x) {
    Node *p = syma_node(x);
    Count i;
    char tmp[12];
    uint32_t c;
    syma_bufput(b, '"');
    for (i = 0; i < p->len; i++) {
        c = p->s[i];
        if (c == '"' || c == '\\') {
            syma_bufput(b, '\\');
            syma_bufput(b, (unsigned)c);
        } else if (c < 32 || (c >= 0xd800 && c <= 0xdfff)) {
            sprintf(tmp, "\\u%04x", (unsigned)c);
            syma_buftext(b, tmp);
        } else
            emitutf(b, c);
    }
    syma_bufput(b, '"');
}
static void render(Buffer *b, Id x, unsigned json, unsigned depth) {
    Node *p = syma_node(x);
    Count i;
    char t[64];
    if (depth > 512)
        syma_fail("print nesting limit exceeded");
    if (!p) {
        syma_buftext(b, json ? "null" : "{}");
        return;
    }
    if (json) {
        syma_buftext(b, "{\"k\":\"");
        syma_buftext(b, p->kind == SYM   ? "Sym"
                        : p->kind == STR ? "Str"
                        : p->kind == NUM ? "Num"
                                         : "Call");
        syma_buftext(b, "\",");
    }
    if (p->kind == CALL) {
        if (json) {
            syma_buftext(b, "\"h\":");
            render(b, p->h, 1, depth + 1);
            syma_buftext(b, ",\"a\":[");
        } else {
            syma_bufput(b, '{');
            if (p->h)
                render(b, p->h, 0, depth + 1);
        }
        for (i = 0; i < syma_length(p->a); i++) {
            if (json) {
                if (i)
                    syma_bufput(b, ',');
            } else if (p->h || i)
                syma_bufput(b, ' ');
            render(b, syma_at(p->a, i), json, depth + 1);
        }
        syma_buftext(b, json ? "]" : "}");
    } else {
        if (json)
            syma_buftext(b, "\"v\":");
        if (p->kind == NUM) {
            JSDTOATempMem temp;
            js_dtoa(t, p->num, 10, 0, JS_DTOA_FORMAT_FREE, &temp);
            syma_buftext(b, t);
        } else if (p->kind == STR || json)
            quoted(b, x);
        else {
            char *s = syma_utf8(x);
            syma_buftext(b, s);
            free(s);
        }
    }
    if (json)
        syma_bufput(b, '}');
}
char *syma_show(Id x, unsigned json) {
    Buffer b = {0};
    render(&b, x, json, 0);
    return b.p;
}
void syma_store_init(void) {
    memset(pages, 0, sizeof(pages));
    next_hash_epoch();
    nextid = 1;
    freelist = live = seqcount = 0;
    sequences = free_sequences = NULL;
    sequence_pages = NULL;
}
static void markseq(Seq *s);
static void mark(Id x) {
    Node *p = syma_node(x);
    if (!p || p->mark)
        return;
    p->mark = 1;
    if (p->kind == CALL || p->kind == SPLICE) {
        mark(p->h);
        markseq(p->a);
    }
}
static void markseq(Seq *s) {
    Count i;
    if (!s || s->mark)
        return;
    s->mark = 1;
    if (s->used) {
        for (i = 0; i < s->used; i++)
            mark(s->items[i]);
    } else {
        markseq(s->l);
        markseq(s->r);
    }
}
void syma_gc(Id root, Id universe, Rules *rules, Id *pins, Count npins) {
    Count i;
    Seq **sp = &sequences, *s;
    Node *p;
    mark(root);
    mark(universe);
    syma_mark_effects(mark);
    syma_mark_repl(mark);
    syma_mark_contexts(mark);
    if (rules)
        for (i = 0; i < rules->n; i++) {
            Rule *r = &rules->v[i];
            mark(r->ast);
            mark(r->name);
            mark(r->lhs);
            mark(r->rhs);
            mark(r->guard);
            mark(r->scope);
            mark(r->with);
            mark(r->let);
            mark(r->key);
        }
    for (i = 0; i < npins; i++)
        mark(pins[i]);
    next_hash_epoch();
    for (i = 1; i < nextid; i++) {
        p = syma_node(i);
        if (!p->kind)
            continue;
        if (p->mark) {
            p->mark = 0;
            p->link = bucket_head(p->hash % HASHN);
            hashes[p->hash % HASHN] = i;
            hash_epochs[p->hash % HASHN] = hash_epoch;
        } else {
            free(p->s);
            memset(p, 0, sizeof(*p));
            p->link = freelist;
            freelist = i;
            live--;
        }
    }
    while ((s = *sp)) {
        if (s->mark) {
            s->mark = 0;
            sp = &s->next;
        } else {
            *sp = s->next;
            s->next = free_sequences;
            free_sequences = s;
            seqcount--;
        }
    }
}
Count syma_live_nodes(void) { return live; }
Count syma_allocated_sequences(void) { return seqcount; }
void syma_store_free(void) {
    syma_match_cleanup();
    syma_visit_cleanup();
    Count i;
    for (i = 1; i < nextid; i++)
        if (syma_node(i)->kind)
            free(syma_node(i)->s);
    for (i = 0; i < MAXPAGES; i++)
        free(pages[i]);
    {
        SeqPage *page, *next;
        for (page = sequence_pages; page; page = next) {
            next = page->next;
            free(page);
        }
    }
    syma_store_init();
}
