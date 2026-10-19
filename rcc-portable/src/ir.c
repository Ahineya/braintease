/* Direct RCC IR lowering for the portable C frontend. See upstream notices. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"
#include "cc.h"

struct value {
    char *text;
    struct type *type;
    unsigned long id;
    enum typequal qual;
};

struct block {
    unsigned long id;
    struct array code, predecessors;
    struct block *next;
    struct block *yes, *no;
    bool terminated, loop;
};

struct func {
    struct decl *decl;
    struct type *type;
    struct block *first, *last;
    struct array allocations;
    struct map gotos;
    unsigned long nextid;
    struct allocation *owned;
};

union arena_alignment {
    long double real;
    long long integer;
    void *pointer;
};

struct allocation {
    struct allocation *next;
    size_t used, capacity;
    union arena_alignment alignment;
    char data[];
};

struct global {
    struct decl *decl;
    struct global *next;
};

struct switchentry {
    unsigned long long number;
    struct block *block;
    struct switchentry *next;
};
static struct global *globals;
static unsigned long blockid, globalid;
static struct func *current;

/* Expression text and values live until their function has been emitted.
   Global symbols outlive functions and are allocated outside this arena. */
static void *own(size_t n)
{
    struct allocation *a;
    void *p;
    size_t capacity, alignment = sizeof(union arena_alignment);
    if (!current)
        return xmalloc(n);
    if (n > SIZE_MAX - (alignment - 1))
        fatal("IR arena size overflow");
    n = (n + alignment - 1) & ~(alignment - 1);
    a = current->owned;
    if (!a || a->capacity - a->used < n) {
        capacity = n > 4096 ? n : 4096;
        if (capacity > SIZE_MAX - sizeof(*a))
            fatal("IR arena size overflow");
        a = xmalloc(sizeof(*a) + capacity);
        a->used = 0;
        a->capacity = capacity;
        a->next = current->owned;
        current->owned = a;
    }
    p = a->data + a->used;
    a->used += n;
    return p;
}

static char *text(const char *format, ...)
{
    char local[512], *s;
    int n;
    va_list ap;
    va_start(ap, format);
    n = vsnprintf(local, sizeof(local), format, ap);
    va_end(ap);
    if (n < 0)
        fatal("cannot format IR");
    s = own((size_t)n + 1);
    va_start(ap, format);
    vsnprintf(s, (size_t)n + 1, format, ap);
    va_end(ap);
    return s;
}

static void append(struct array *a, const char *s)
{
    arrayaddbuf(a, s, strlen(s));
}

static void emit(struct func *f, const char *format, ...)
{
    char buf[4096];
    int n;
    va_list ap;
    if (f->last->terminated)
        return;
    va_start(ap, format);
    n = vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(buf))
        fatal("IR instruction exceeds buffer");
    append(&f->last->code, "      ");
    append(&f->last->code, buf);
    append(&f->last->code, "\n");
}

static bool aggregate(struct type *t)
{
    return t->kind == TYPESTRUCT || t->kind == TYPEUNION || t->kind == TYPEARRAY;
}

static bool floating(struct type *t)
{
    return t->prop & PROPFLOAT;
}

static const char *irtype(struct type *t)
{
    if (t->kind == TYPEVOID)
        return "void";
    if (t->kind == TYPEPOINTER || t->kind == TYPENULLPTR)
        return "i16*";
    if (aggregate(t))
        return text("[%llu x i16]", t->size);
    if (floating(t)) {
        if (t->kind == TYPELDOUBLE)
            error(&tok.loc, "portable IR: long double lowering is not implemented");
        return t->kind == TYPEFLOAT ? "f32" : "f64";
    }
    if (t->size == 1)
        return "i16";
    if (t->size == 2)
        return "i32";
    if (t->size == 4)
        return "i64";
    error(&tok.loc, "portable IR: unsupported scalar type");
    return NULL;
}

/* Storage is cell arrays; ABI types retain RCC's structural return layout so
 * separately compiled modules from the two frontends have matching signatures.
 * A repeated struct behind a pointer is an incomplete forward declaration. */
struct typepath {
    struct type *type;
    const struct typepath *parent;
};

static const char *layouttype(struct type *t, const struct typepath *path)
{
    struct member *m;
    const struct typepath *p;
    struct typepath here = {t, path};
    struct array fields = {0};
    char *result;
    unsigned long long end = 0;
    if (t->kind == TYPEPOINTER)
        return text("%s*", layouttype(t->base, path));
    if (t->kind == TYPEFUNC)
        return "i16*";
    if (t->kind == TYPEARRAY)
        return text("[%llu x %s]", t->base->size ? t->size / t->base->size : 0,
                    layouttype(t->base, path));
    if (t->kind != TYPESTRUCT)
        return irtype(t);
    append(&fields,
           t->u.structunion.tag ? text("struct \"%s\" {", t->u.structunion.tag) : "struct {");
    for (p = path; p; p = p->parent)
        if (p->type == t)
            break;
    if (!p)
        for (m = t->u.structunion.members; m; m = m->next) {
            if (m->offset < end || !m->type->size)
                continue;
            if (end)
                append(&fields, ", ");
            append(&fields, layouttype(m->type, &here));
            end = m->offset + m->type->size;
        }
    append(&fields, "}");
    result = own(fields.len + 1);
    memcpy(result, fields.val, fields.len);
    result[fields.len] = 0;
    free(fields.val);
    return result;
}

static const char *resulttype(struct type *t)
{
    return aggregate(t) ? layouttype(t, NULL) : irtype(t);
}

static struct value *value(struct type *t, char *s)
{
    struct value *v = own(sizeof(*v));
    v->type = t;
    v->text = s;
    v->id = 0;
    v->qual = QUALNONE;
    return v;
}

static struct value *fresh(struct func *f, struct type *t)
{
    struct value *v = value(t, NULL);
    if (f->nextid >= 0xffffffffUL)
        error(&tok.loc, "too many RCC IR temporaries");
    v->id = f->nextid++;
    v->text = t->kind == TYPEPOINTER ? text("fat(%%%lu, mixed)", v->id) : text("%%%lu", v->id);
    return v;
}

struct value *mkintconst(unsigned long long n)
{
    return value(&typeullong, text("%lld", (long long)n));
}

struct block *mkblock(char *name)
{
    struct block *b = xmalloc(sizeof(*b));
    memset(b, 0, sizeof(*b));
    b->id = ++blockid;
    b->loop = !strcmp(name, "while_cond") || !strcmp(name, "for_cond") || !strcmp(name, "do_cond");
    return b;
}

struct value *mkglobal(struct decl *d)
{
    struct global *g;
    struct func *saved = current;
    const char *name;
    if (d->value)
        return d->value;
    current = NULL;
    name = d->asmname ? d->asmname : d->name;
    if (strchr(name, '"') || strchr(name, '\\') || strchr(name, '\n'))
        error(&tok.loc, "portable IR: unsupported assembler symbol spelling");
    if (d->kind == DECLOBJECT && d->u.obj.storage == SDTHREAD)
        error(&tok.loc, "portable IR: thread storage is not implemented");
    if (d->linkage == LINKNONE)
        name = text("__rcc_c_%lu_%s", ++globalid, name);
    g = xmalloc(sizeof(*g));
    g->decl = d;
    g->next = globals;
    globals = g;
    d->value = value(mkpointertype(d->type, QUALNONE),
                     d->kind == DECLFUNC ? text("func @\"%s\"", name)
                                         : text("fat(global @\"%s\", global)", name));
    current = saved;
    return d->value;
}

static char *symbol(struct value *v)
{
    /* Names originate in C identifiers/assembler names. Reject unsafe quoting. */
    char *start = strchr(v->text, '"'), *end = start ? strchr(start + 1, '"') : NULL, *s;
    size_t n;
    if (!start || !end)
        fatal("invalid global IR value");
    n = (size_t)(end - start - 1);
    s = own(n + 1);
    memcpy(s, start + 1, n);
    s[n] = 0;
    return s;
}

static struct value *unary(struct func *f, const char *op, struct type *t, struct value *a)
{
    struct value *v = fresh(f, t);
    emit(f, "%%%lu = %s %s %s", v->id, op, irtype(t), a->text);
    return v;
}

static struct value *intrinsic(struct func *f, const char *name, struct type *t, struct value *a,
                               struct value *b)
{
    struct value *v = fresh(f, t);
    emit(f, "%%%lu = intrinsic %s @\"%s\"(%s%s%s)", v->id, irtype(t), name, a->text, b ? ", " : "",
         b ? b->text : "");
    return v;
}

static struct value *binary(struct func *f, const char *op, struct type *t, struct value *a,
                            struct value *b)
{
    struct value *v = fresh(f, t);
    emit(f, "%%%lu = %s %s %s, %s", v->id, op, irtype(t), a->text, b->text);
    return v;
}

static struct value *convert(struct func *f, struct type *to, struct type *from, struct value *v)
{
    const char *op, *precision;
    struct type *middle;
    if (to->kind == TYPEVOID)
        return v;
    if (to == &typebool) {
        if (floating(from)) {
            const char *precision = from->kind == TYPEFLOAT ? "f32" : "f64";
            struct value *zero =
                intrinsic(f, text("float.value.%s", precision), from, mkintconst(0), NULL);
            return intrinsic(f, text("float.%s.ne", precision), &typeint, v, zero);
        }
        if (from->kind == TYPEPOINTER)
            v = unary(f, "ptrtoint", &typeulong, v);
        return binary(f, "ne", &typeint, v, mkintconst(0));
    }
    if (to->kind == TYPEPOINTER) {
        if (from->kind == TYPEPOINTER)
            return v;
        if ((from->prop & PROPINT) && from->size < 2)
            v = convert(f, &typeulong, from, v);
        return unary(f, "inttoptr", to, v);
    }
    if (from->kind == TYPEPOINTER) {
        v = unary(f, "ptrtoint", &typeulong, v);
        from = &typeulong;
    }
    if (floating(to) || floating(from)) {
        if (floating(to) && floating(from)) {
            if (to->kind == from->kind)
                return v;
            op = from->kind == TYPEFLOAT ? "from_f32" : "from_f64";
            precision = to->kind == TYPEFLOAT ? "f32" : "f64";
        } else if (floating(to)) {
            if (from->size < 2) {
                v = convert(f, from->u.arith.issigned ? &typelong : &typeulong, from, v);
                from = from->u.arith.issigned ? &typelong : &typeulong;
            }
            op = from->size == 4 ? (from->u.arith.issigned ? "from_i64" : "from_u64")
                                 : (from->u.arith.issigned ? "from_si" : "from_ui");
            precision = to->kind == TYPEFLOAT ? "f32" : "f64";
        } else {
            op = to->size == 4 ? (to->u.arith.issigned ? "to_i64" : "to_u64")
                               : (to->u.arith.issigned ? "to_si" : "to_ui");
            middle = to->size == 4 ? to : (to->u.arith.issigned ? &typelong : &typeulong);
            v = intrinsic(f, text("float.%s.%s", from->kind == TYPEFLOAT ? "f32" : "f64", op),
                          middle, v, NULL);
            return convert(f, to, middle, v);
        }
        return intrinsic(f, text("float.%s.%s", precision, op), to, v, NULL);
    }
    if (to->size == from->size)
        return v;
    /* Constants have no source-width tag in RCC IR. Give wide values a typed
       temporary before extending so the backend uses the source sign bit. */
    if (from->size > 1 && (v->text[0] == '-' || (v->text[0] >= '0' && v->text[0] <= '9')))
        v = binary(f, "add", from, v, mkintconst(0));
    op = to->size < from->size ? "trunc" : from->u.arith.issigned ? "sext" : "zext";
    return unary(f, op, to, v);
}

static struct value *gep(struct func *f, struct value *p, struct value *offset)
{
    struct value *v = fresh(f, mkpointertype(&typechar, QUALNONE));
    emit(f, "%%%lu = getelementptr i16*, i16 %s, %s", v->id, p->text, offset->text);
    return v;
}

static struct value *temporary(struct func *f, struct type *t)
{
    struct value *v = fresh(f, mkpointertype(t, QUALNONE));
    if (!t->size)
        error(&tok.loc, "portable IR: variable length allocation is not implemented");
    append(&f->allocations, text("      %%%lu = alloca %s -> i16*\n", v->id, irtype(t)));
    return v;
}

static struct value *allocate(struct func *f, struct decl *d)
{
    struct value *v;
    if (d->value)
        return d->value;
    if (!d->type->size)
        error(&tok.loc, "portable IR: variable length allocation is not implemented");
    if (d->u.obj.align > 1)
        error(&tok.loc, "portable IR: extended alignment is not implemented");
    v = temporary(f, d->type);
    d->value = v;
    return v;
}

static struct value *load(struct func *f, struct type *t, enum typequal q, struct value *p)
{
    struct value *v;
    if (aggregate(t)) {
        v = value(t, p->text);
        v->qual = q;
        return v;
    }
    v = fresh(f, t);
    emit(f, "%%%lu = load %s%s, %s", v->id, q & QUALVOLATILE ? "volatile " : "", irtype(t),
         p->text);
    return v;
}

static bool volatilecell(struct type *t, enum typequal q, unsigned long long offset)
{
    struct member *m;
    if (q & QUALVOLATILE)
        return true;
    if (t->kind == TYPEARRAY && t->base->size)
        return volatilecell(t->base, t->qual, offset % t->base->size);
    if (t->kind == TYPESTRUCT || t->kind == TYPEUNION)
        for (m = t->u.structunion.members; m; m = m->next)
            if (offset >= m->offset && offset - m->offset < m->type->size &&
                volatilecell(m->type, m->qual, offset - m->offset))
                return true;
    return false;
}

static void store(struct func *f, struct type *t, enum typequal q, struct value *p, struct value *v)
{
    unsigned long long i;
    if (aggregate(t)) {
        for (i = 0; i < t->size; i++)
            store(f, &typeushort, volatilecell(t, q, i) ? QUALVOLATILE : QUALNONE,
                  gep(f, p, mkintconst(i)),
                  load(f, &typeushort, volatilecell(t, v->qual, i) ? QUALVOLATILE : QUALNONE,
                       gep(f, v, mkintconst(i))));
    } else
        emit(f, "store %s%s %s, %s", q & QUALVOLATILE ? "volatile " : "", irtype(t), v->text,
             p->text);
}

static struct value *snapshot(struct func *f, struct type *t, struct value *source)
{
    struct value *dest = temporary(f, t);
    store(f, t, QUALNONE, dest, source);
    return dest;
}

static struct value *lvalue(struct func *f, struct expr *e)
{
    switch (e->kind) {
    case EXPRIDENT:
        return e->u.ident.decl->value;
    case EXPRSTRING:
        return stringdecl(e)->value;
    case EXPRUNARY:
        if (e->op == TMUL)
            return funcexpr(f, e->base);
        break;
    case EXPRCOMPOUND:
        funcinit(f, e->u.compound.decl, e->u.compound.init, true);
        return e->u.compound.decl->value;
    }
    /* Member access takes the address of struct rvalues internally. */
    if (aggregate(e->type))
        return funcexpr(f, e);
    error(&tok.loc, "portable IR: unsupported lvalue (including bitfields)");
    return NULL;
}

static void edge(struct func *f, struct block *to)
{
    unsigned long *p = arrayadd(&to->predecessors, sizeof(*p));
    *p = f->last->id;
    /* Structured loop conditions are marked at construction. Goto targets
       also need polling: their backedges are not structured loop callbacks. */
}

void funcjmp(struct func *f, struct block *b)
{
    if (f->last->terminated)
        return;
    emit(f, "br label %lu", b->id);
    edge(f, b);
    f->last->yes = b;
    f->last->terminated = true;
}

void funclabel(struct func *f, struct block *b)
{
    funcjmp(f, b);
    f->last->next = b;
    f->last = b;
}

void funcjnz(struct func *f, struct value *v, struct type *t, struct block *yes, struct block *no)
{
    if (f->last->terminated)
        return;
    v = convert(f, &typebool, t ? t : v->type, v);
    emit(f, "br %s, label %lu, label %lu", v->text, yes->id, no->id);
    edge(f, yes);
    edge(f, no);
    f->last->yes = yes;
    f->last->no = no;
    f->last->terminated = true;
}

struct value *funcbranch(struct func *f, struct expr *e, struct block *yes, struct block *no)
{
    struct block *next;
    struct value *v;
    if (e->kind == EXPRBINARY && (e->op == TLAND || e->op == TLOR)) {
        next = mkblock("logic");
        funcbranch(f, e->u.binary.l, e->op == TLAND ? next : yes, e->op == TLAND ? no : next);
        funclabel(f, next);
        return funcbranch(f, e->u.binary.r, yes, no);
    }
    v = funcexpr(f, e);
    funcjnz(f, v, e->type, yes, no);
    return v;
}

void funcret(struct func *f, struct value *v)
{
    if (f->last->terminated)
        return;
    if (v && aggregate(f->type->base))
        v = snapshot(f, f->type->base, v);
    emit(f, "ret %s", v ? v->text : "void");
    f->last->terminated = true;
}

void funchlt(struct func *f)
{
    emit(f, "ret %s", f->type->base == &typevoid ? "void" : "0");
    f->last->terminated = true;
}

struct gotolabel *funcgoto(struct func *f, char *name)
{
    struct mapkey key;
    size_t i;
    struct gotolabel *g;
    mapkey(&key, name, strlen(name));
    if (mapput(&f->gotos, &key, &i)) {
        g = xmalloc(sizeof(*g));
        g->defined = false;
        g->label = mkblock(name);
        g->label->loop = true;
        f->gotos.vals[i].p = g;
    }
    return f->gotos.vals[i].p;
}

void switchcase(struct switchcases *c, unsigned long long n, struct block *b)
{
    struct switchentry *e;
    for (e = c->root; e; e = e->next)
        if (e->number == n)
            error(&tok.loc, "duplicate switch case");
    e = own(sizeof(*e));
    e->number = n;
    e->block = b;
    e->next = c->root;
    c->root = e;
}

void funcswitch(struct func *f, struct value *v, struct switchcases *c, struct block *fallback)
{
    struct switchentry *e = c->root;
    while (e) {
        struct block *next = mkblock("switch");
        funcjnz(f, binary(f, "eq", &typeint, v, mkintconst(e->number)), &typeint, e->block, next);
        funclabel(f, next);
        e = e->next;
    }
    funcjmp(f, c->defaultlabel ? c->defaultlabel : fallback);
}

struct type *functype(struct func *f)
{
    return f->type;
}

struct func *mkfunc(struct decl *d, char *name, struct type *t, struct scope *s)
{
    struct func *f = xmalloc(sizeof(*f));
    struct decl *p;
    unsigned long n = 0;
    (void)name;
    (void)s;
    memset(f, 0, sizeof(*f));
    f->decl = d;
    f->type = t;
    f->first = f->last = mkblock("entry");
    mapinit(&f->gotos, 8);
    current = f;
    f->nextid = t->u.func.nparam;
    for (p = t->u.func.params; p; p = p->next, n++) {
        if (p->name)
            store(f, p->type, p->qual, allocate(f, p),
                  value(p->type,
                        aggregate(p->type) ? text("fat(%%%lu, mixed)", n) : text("%%%lu", n)));
    }
    return f;
}

static struct value *conditional(struct func *f, struct expr *e)
{
    struct block *yes = mkblock("true"), *no = mkblock("false"), *join = mkblock("join");
    struct block *a, *b;
    struct value *x, *y, *v;
    funcbranch(f, e->base, yes, no);
    funclabel(f, yes);
    x = funcexpr(f, e->u.cond.t);
    if (e->type != &typevoid)
        x = convert(f, e->type, e->u.cond.t->type, x);
    a = f->last;
    funcjmp(f, join);
    funclabel(f, no);
    y = funcexpr(f, e->u.cond.f);
    if (e->type != &typevoid)
        y = convert(f, e->type, e->u.cond.f->type, y);
    b = f->last;
    funcjmp(f, join);
    funclabel(f, join);
    if (e->type == &typevoid)
        return NULL;
    v = fresh(f, aggregate(e->type) ? mkpointertype(e->type, QUALNONE) : e->type);
    emit(f, "%%%lu = phi %s [%s, label %lu], [%s, label %lu]", v->id, irtype(v->type), x->text,
         a->id, y->text, b->id);
    return v;
}

struct value *funcexpr(struct func *f, struct expr *e)
{
    struct value *l, *r, *v, *p;
    struct expr *arg;
    struct type *t;
    const char *op = NULL;
    size_t i;
    if (!e)
        return NULL;
    switch (e->kind) {
    case EXPRIDENT:
        if (e->u.ident.decl->kind == DECLFUNC || e->u.ident.decl->kind == DECLCONST)
            return e->u.ident.decl->value;
        return load(f, e->type, e->qual, e->u.ident.decl->value);
    case EXPRCONST:
        if (e->type->kind == TYPEPOINTER)
            return unary(f, "inttoptr", e->type, mkintconst(e->u.constant.u & 0xffffffffULL));
        if (floating(e->type)) {
            union {
                double d;
                uint64_t bits;
            } d;

            union {
                float f;
                uint32_t bits;
            } s;

            if (e->type->kind == TYPEFLOAT) {
                s.f = (float)e->u.constant.f;
                return intrinsic(f, "float.value.f32", e->type, mkintconst(s.bits), NULL);
            }
            d.d = e->u.constant.f;
            return intrinsic(f, "float.value.f64", e->type, mkintconst(d.bits), NULL);
        }
        return value(e->type, text("%lld", (long long)e->u.constant.u));
    case EXPRSTRING:
        return stringdecl(e)->value;
    case EXPRCOMPOUND:
        return lvalue(f, e);
    case EXPRUNARY:
        if (e->op == TBAND)
            return lvalue(f, e->base);
        if (e->op == TMUL)
            return load(f, e->type, e->qual, funcexpr(f, e->base));
        if (e->op == TSUB) {
            r = funcexpr(f, e->base);
            if (floating(e->type)) {
                const char *precision = e->type->kind == TYPEFLOAT ? "f32" : "f64";
                struct type *bits = e->type->kind == TYPEFLOAT ? &typeulong : &typeullong;
                /* Flip the sign bit, preserving signed zero and NaN payloads.
                 * Arithmetic 0-x also loses zero's sign and an untyped zero
                 * would occupy only one word at the Ripple helper-call ABI. */
                r = intrinsic(f, text("float.bits.%s", precision), bits, r, NULL);
                r = binary(
                    f, "xor", bits, r,
                    mkintconst(e->type->kind == TYPEFLOAT ? 0x80000000ULL : 0x8000000000000000ULL));
                return intrinsic(f, text("float.value.%s", precision), e->type, r, NULL);
            }
            return unary(f, "neg", e->type, r);
        }
        break;
    case EXPRCAST:
        if (e->toeval)
            funcexpr(f, e->toeval);
        return convert(f, e->type, e->base->type, funcexpr(f, e->base));
    case EXPRASSIGN:
        r = funcexpr(f, e->u.assign.r);
        if (e->u.assign.l->kind == EXPRTEMP)
            e->u.assign.l->u.temp = r;
        else
            store(f, e->u.assign.l->type, e->u.assign.l->qual, lvalue(f, e->u.assign.l), r);
        return r;
    case EXPRTEMP:
        return e->u.temp;
    case EXPRCOMMA:
        for (arg = e->base; arg; arg = arg->next)
            v = funcexpr(f, arg);
        return v;
    case EXPRINCDEC:
        p = lvalue(f, e->base);
        l = load(f, e->type, e->qual, p);
        if (e->type->kind == TYPEPOINTER)
            r = gep(f, l,
                    mkintconst(e->op == TINC ? e->type->base->size : 0 - e->type->base->size));
        else if (floating(e->type)) {
            struct expr one = {.kind = EXPRCONST, .type = e->type, .u.constant.f = 1};
            r = intrinsic(f,
                          text("float.%s.%s", e->type->kind == TYPEFLOAT ? "f32" : "f64",
                               e->op == TINC ? "add" : "sub"),
                          e->type, l, funcexpr(f, &one));
        } else
            r = binary(f, e->op == TINC ? "add" : "sub", e->type, l, mkintconst(1));
        store(f, e->type, e->qual, p, r);
        return e->u.incdec.post ? l : r;
    case EXPRCALL: {
        struct array args = {0};
        struct type *ft = e->base->type->base;
        for (arg = e->u.call.args; arg; arg = arg->next) {
            r = funcexpr(f, arg);
            if (aggregate(arg->type))
                r = snapshot(f, arg->type, r);
            /* Untyped constants otherwise consume a single ABI slot. */
            else if ((arg->type->prop & PROPINT) && arg->type->size > 1 &&
                     (r->text[0] == '-' || (r->text[0] >= '0' && r->text[0] <= '9')))
                r = binary(f, "add", arg->type, r, mkintconst(0));
            arrayaddptr(&args, r);
        }
        l = funcexpr(f, e->base);
        v = e->type == &typevoid
                ? NULL
                : fresh(f, aggregate(e->type) ? mkpointertype(e->type, QUALNONE) : e->type);
        if (v)
            emit(f, "%%%lu = call %s %s()", v->id, resulttype(e->type), l->text);
        else
            emit(f, "call void %s()", l->text);
        /* Replace the empty argument list in the last instruction. */
        if (!f->last->terminated) {
            f->last->code.len -= 2; /* ')' and newline */
            for (i = 0; i < e->u.call.nargs; i++) {
                if (i)
                    append(&f->last->code, ", ");
                append(&f->last->code, ((struct value **)args.val)[i]->text);
            }
            append(&f->last->code, ")");
            if (ft->u.func.isvararg)
                append(&f->last->code, text(" stack %zu", ft->u.func.nparam));
            append(&f->last->code, "\n");
        }
        free(args.val);
        return aggregate(e->type) ? snapshot(f, e->type, v) : v;
    }
    case EXPRCOND:
        return conditional(f, e);
    case EXPRBINARY:
        if (e->op == TLAND || e->op == TLOR) {
            struct block *yes = mkblock("yes"), *no = mkblock("no"), *join = mkblock("join");
            funcbranch(f, e, yes, no);
            funclabel(f, yes);
            funcjmp(f, join);
            funclabel(f, no);
            funcjmp(f, join);
            funclabel(f, join);
            v = fresh(f, &typeint);
            emit(f, "%%%lu = phi i16 [1, label %lu], [0, label %lu]", v->id, yes->id, no->id);
            return v;
        }
        if (e->pointer_difference) {
            l = unary(f, "ptrtolinear", &typelong, funcexpr(f, e->u.binary.l->base));
            r = unary(f, "ptrtolinear", &typelong, funcexpr(f, e->u.binary.r->base));
            return binary(f, "sub", &typelong, l, r);
        }
        /* cproc expresses member addresses as integer addition and then
           retags the result as a pointer. Preserve the pointer before adding
           the cell offset so fields crossing a bank use GEP normalization. */
        if (e->type->kind == TYPEPOINTER && e->op == TADD && e->u.binary.l->kind == EXPRCAST &&
            e->u.binary.l->base->type->kind == TYPEPOINTER)
            return gep(f, funcexpr(f, e->u.binary.l->base), funcexpr(f, e->u.binary.r));
        l = funcexpr(f, e->u.binary.l);
        r = funcexpr(f, e->u.binary.r);
        t = e->u.binary.l->type;
        if (t->kind == TYPEPOINTER && (e->op == TADD || e->op == TSUB))
            return gep(f, l, e->op == TSUB ? unary(f, "neg", &typelong, r) : r);
        if (t->kind == TYPEPOINTER) {
            l = unary(f, "ptrtoint", &typeulong, l);
            r = unary(f, "ptrtoint", &typeulong, r);
            t = &typeulong;
        }
        switch (e->op) {
        case TADD:
            op = "add";
            break;
        case TSUB:
            op = "sub";
            break;
        case TMUL:
            op = "mul";
            break;
        case TDIV:
            op = t->u.arith.issigned ? "sdiv" : "udiv";
            break;
        case TMOD:
            op = t->u.arith.issigned ? "srem" : "urem";
            break;
        case TBAND:
            op = "and";
            break;
        case TBOR:
            op = "or";
            break;
        case TXOR:
            op = "xor";
            break;
        case TSHL:
            op = "shl";
            break;
        case TSHR:
            op = t->u.arith.issigned ? "ashr" : "lshr";
            break;
        case TEQL:
            op = "eq";
            break;
        case TNEQ:
            op = "ne";
            break;
        case TLESS:
            op = (t->prop & PROPINT) && t->u.arith.issigned ? "slt" : "ult";
            break;
        case TGREATER:
            op = (t->prop & PROPINT) && t->u.arith.issigned ? "sgt" : "ugt";
            break;
        case TLEQ:
            op = (t->prop & PROPINT) && t->u.arith.issigned ? "sle" : "ule";
            break;
        case TGEQ:
            op = (t->prop & PROPINT) && t->u.arith.issigned ? "sge" : "uge";
            break;
        default:
            break;
        }
        if (floating(t)) {
            switch (e->op) {
            case TDIV:
                op = "div";
                break;
            case TLESS:
                op = "lt";
                break;
            case TGREATER:
                op = "gt";
                break;
            case TLEQ:
                op = "le";
                break;
            case TGEQ:
                op = "ge";
                break;
            default:
                break;
            }
            if (op)
                return intrinsic(f, text("float.%s.%s", t->kind == TYPEFLOAT ? "f32" : "f64", op),
                                 e->type, l, r);
        }
        if (op)
            return binary(f, op, e->type, l, r);
        break;
    case EXPRBUILTIN:
        if (e->u.builtin.kind == BUILTINUNREACHABLE) {
            funchlt(f);
            return NULL;
        }
        if (e->u.builtin.kind == BUILTINVASTART) {
            if (!f->type->u.func.isvararg)
                error(&tok.loc, "portable IR: va_start requires a variadic function");
            p = funcexpr(f, e->base);
            emit(f, "intrinsic void @\"va_start\"(%s)", p->text);
            return NULL;
        }
        if (e->u.builtin.kind == BUILTINVAARG) {
            if (!e->type->size || e->type->incomplete)
                error(&tok.loc, "portable IR: va_arg requires a complete object type");
            p = funcexpr(f, e->base);
            /* Aggregate extras use the same pointer carrier as fixed arguments. */
            v = intrinsic(f, "va_arg",
                          aggregate(e->type) ? mkpointertype(e->type, QUALNONE) : e->type, p, NULL);
            return aggregate(e->type) ? snapshot(f, e->type, v) : v;
        }
        break;
    default:
        break;
    }
    error(&tok.loc, "portable IR: expression lowering is not implemented (kind %d)", e->kind);
    return NULL;
}

void funcinit(struct func *f, struct decl *d, struct init *init, bool hasinit)
{
    unsigned long long i;
    struct value *p = allocate(f, d);
    if (hasinit && aggregate(d->type))
        for (i = 0; i < d->type->size; i++)
            store(f, &typeushort, QUALNONE, gep(f, p, mkintconst(i)), mkintconst(0));
    for (; init; init = init->next) {
        struct value *at = p;
        if (init->bits.before || init->bits.after)
            error(&tok.loc, "portable IR: bitfield initialization is not implemented");
        if (init->start)
            at = gep(f, p, mkintconst(init->start));
        if (init->expr->kind == EXPRSTRING) {
            const uint_least16_t *s = init->expr->u.string.data;
            for (i = 0; i < init->end - init->start && i < init->expr->u.string.size; i++)
                store(f, &typeushort, QUALNONE, gep(f, at, mkintconst(i)), mkintconst(s[i]));
        } else
            store(f, init->expr->type, QUALNONE, at, funcexpr(f, init->expr));
    }
}

static void signature(struct decl *d, bool external)
{
    struct decl *p;
    unsigned long n = 0;
    struct type *t = d->type;
    printf("  function%s%s %s @\"%s\"(", external ? " external" : "",
           t->u.func.isvararg ? " vararg" : "", resulttype(t->base), symbol(d->value));
    for (p = t->u.func.params; p; p = p->next, n++)
        printf("%s%s %%%lu", n ? ", " : "", aggregate(p->type) ? "i16*" : irtype(p->type), n);
    puts(") {");
}

void emitfunc(struct func *f, bool exported)
{
    struct block *b;
    unsigned long *pred;
    struct mapkey *key;
    size_t i;
    for (i = 0; i < f->gotos.cap; i++) {
        key = &f->gotos.keys[i];
        if (key->str && !((struct gotolabel *)f->gotos.vals[i].p)->defined)
            error(&tok.loc, "undefined goto label");
    }
    if (!f->last->terminated) {
        if (f->type->base == &typevoid)
            funcret(f, NULL);
        /* The parser can leave an unreachable join after two returning arms.
         * Also choose zero for non-void fallthrough (undefined if used in C,
         * except for main, where zero is required). */
        else
            funchlt(f);
    }
    if (!exported)
        printf("  internal function @\"%s\"\n", symbol(f->decl->value));
    signature(f->decl, false);
    for (b = f->first; b; b = b->next) {
        printf("    block %lu%s", b->id, b->loop ? " loop" : "");
        if (b->predecessors.len) {
            fputs(" preds ", stdout);
            arrayforeach(&b->predecessors, pred)
                printf("%s%lu", pred == (unsigned long *)b->predecessors.val ? "" : ", ", *pred);
        }
        if (b->yes)
            printf(" succs %lu", b->yes->id);
        if (b->no)
            printf(", %lu", b->no->id);
        puts(":");
        if (b == f->first && f->allocations.len)
            fwrite(f->allocations.val, 1, f->allocations.len, stdout);
        if (b->code.len)
            fwrite(b->code.val, 1, b->code.len, stdout);
    }
    puts("  }");
}

void delfunc(struct func *f)
{
    struct block *b, *next;
    struct allocation *a, *following;
    for (b = f->first; b; b = next) {
        next = b->next;
        free(b->code.val);
        free(b->predecessors.val);
        free(b);
    }
    free(f->allocations.val);
    mapfree(&f->gotos, free);
    for (a = f->owned; a; a = following) {
        following = a->next;
        free(a);
    }
    current = NULL;
    free(f);
}

static unsigned long long bits(struct expr *e)
{
    union {
        double d;
        uint64_t u;
    } d;

    union {
        float f;
        uint32_t u;
    } s;

    e = eval(e);
    if (e->kind != EXPRCONST)
        error(&tok.loc, "portable IR: initializer must be constant");
    if (floating(e->type)) {
        if (e->type->kind == TYPEFLOAT) {
            s.f = (float)e->u.constant.f;
            return s.u;
        }
        d.d = e->u.constant.f;
        return d.u;
    }
    return e->u.constant.u;
}

/* Address displacements are in cells, already scaled by the frontend. Keep
 * symbols unresolved until linking and normalize numeric pointers only when
 * arithmetic is performed; their integer casts otherwise preserve raw bits. */
struct address {
    struct decl *base;
    unsigned long long bits;
    long long cells;
};

static bool hasaddress(struct expr *e);
static struct address addressconstant(struct expr *e);

static struct address objectaddress(struct expr *e)
{
    struct decl *d;
    if (e->kind == EXPRUNARY && e->op == TMUL)
        return addressconstant(e->base);
    if (e->kind == EXPRCOMPOUND)
        e = eval(e);
    if (e->kind == EXPRSTRING)
        d = stringdecl(e);
    else if (e->kind == EXPRIDENT)
        d = e->u.ident.decl;
    else {
        error(&tok.loc, "portable IR: initializer requires a static object address");
        return (struct address){0};
    }
    if (d->kind != DECLFUNC && (d->kind != DECLOBJECT || d->u.obj.storage != SDSTATIC))
        error(&tok.loc, "portable IR: initializer requires a static object address");
    mkglobal(d);
    return (struct address){.base = d};
}

static struct address addressconstant(struct expr *e)
{
    struct address a;
    struct expr *n;
    long long displacement;
    if (e->kind == EXPRCAST) {
        if (!hasaddress(e->base)) {
            n = eval(e->base);
            if (n->kind != EXPRCONST || floating(n->type))
                error(&tok.loc, "portable IR: initializer must be constant");
            /* Evaluate integer casts before converting their bits to a pointer. */
            if (e->type->kind != TYPEPOINTER)
                n = eval(e);
            return (struct address){.bits = n->u.constant.u & 0xffffffffULL};
        }
        if (e->type->kind != TYPEPOINTER && (!(e->type->prop & PROPINT) || e->type->size != 2))
            error(&tok.loc,
                  "portable IR: address initializer cast requires a pointer or 32-bit integer");
        return addressconstant(e->base);
    }
    if (e->kind == EXPRUNARY && e->op == TBAND)
        return objectaddress(e->base);
    if (e->kind == EXPRIDENT && e->u.ident.decl->kind == DECLFUNC)
        return objectaddress(e);
    if (e->kind == EXPRCOND) {
        n = eval(e->base);
        if (n->kind != EXPRCONST)
            error(&tok.loc, "portable IR: initializer condition must be constant");
        return addressconstant((floating(n->type) ? n->u.constant.f != 0.0 : n->u.constant.u != 0)
                                   ? e->u.cond.t
                                   : e->u.cond.f);
    }
    if (e->kind == EXPRBINARY && (e->op == TADD || e->op == TSUB) && hasaddress(e)) {
        a = addressconstant(e->u.binary.l);
        n = eval(e->u.binary.r);
        if (n->kind != EXPRCONST || !(n->type->prop & PROPINT) ||
            a.base && a.base->kind == DECLFUNC)
            error(&tok.loc, "portable IR: invalid address displacement");
        displacement = (long long)n->u.constant.u;
        if (e->op == TSUB) {
            if (displacement == LLONG_MIN)
                error(&tok.loc, "portable IR: address displacement overflow");
            displacement = -displacement;
        }
        if ((displacement > 0 && a.cells > LLONG_MAX - displacement) ||
            (displacement < 0 && a.cells < LLONG_MIN - displacement))
            error(&tok.loc, "portable IR: address displacement overflow");
        a.cells += displacement;
        return a;
    }
    e = eval(e);
    if (e->kind != EXPRCONST || floating(e->type))
        error(&tok.loc, "portable IR: initializer must be constant");
    return (struct address){.bits = e->u.constant.u & 0xffffffffULL};
}

static char *addressvalue(struct expr *e)
{
    struct address a = addressconstant(e);
    const char *name;
    unsigned long long encoded, linear;
    long long at;
    if (a.base) {
        name = symbol(a.base->value);
        if (a.base->kind == DECLFUNC)
            return text("func @\"%s\"", name);
        if (a.cells)
            return text("fat(offset(global @\"%s\", %lld), global)", name, a.cells);
        return text("fat(global @\"%s\", global)", name);
    }
    encoded = a.bits;
    if (a.cells) {
        linear = (encoded >> 16) * 64000 + (encoded & 65535);
        if (a.cells < -(long long)linear || a.cells > 4194303999LL - (long long)linear)
            error(&tok.loc, "portable IR: numeric address displacement is outside logical memory");
        at = (long long)linear + a.cells;
        encoded = ((unsigned long long)at / 64000 << 16) | (unsigned long long)at % 64000;
    }
    return encoded ? text("fat(%llu, heap(%llu))", encoded & 65535, encoded >> 16)
                   : text("fat(0, null)");
}

static bool hasaddress(struct expr *e)
{
    if (e->type->kind == TYPEPOINTER)
        return true;
    switch (e->kind) {
    case EXPRUNARY:
        return e->op == TBAND;
    case EXPRCAST:
        return hasaddress(e->base);
    case EXPRBINARY:
        return hasaddress(e->u.binary.l) || hasaddress(e->u.binary.r);
    case EXPRCOND:
        return hasaddress(e->u.cond.t) || hasaddress(e->u.cond.f);
    case EXPRIDENT:
        return e->u.ident.decl->kind == DECLFUNC;
    default:
        return false;
    }
}

static char *constantvalue(struct expr *e)
{
    if (e->type->kind == TYPEPOINTER || hasaddress(e)) {
        if (e->type->size != 2)
            error(&tok.loc, "portable IR: address initializer requires two cells");
        return addressvalue(e);
    }
    return text("%lld", (long long)bits(e));
}

static void datafield(struct array *types, struct array *values, const char *type,
                      const char *literal)
{
    if (types->len) {
        append(types, ", ");
        append(values, ", ");
    }
    append(types, type);
    append(values, literal);
}

void emitdata(struct decl *d, struct init *init)
{
    struct init *item;
    struct array types = {0}, values = {0}, chars = {0};
    const char *literal = NULL;
    unsigned long long i, n = d->type->size, at = 0, length;
    if (d->u.obj.storage == SDTHREAD || d->u.obj.align > 1)
        error(&tok.loc, "portable IR: thread storage and extended alignment are not implemented");
    /* Evaluate before printing the header: strings and file-scope compound
     * literals can emit additional globals while resolving an initializer. */
    if (init && !aggregate(d->type))
        literal = constantvalue(init->expr);
    else if (init) {
        for (item = init; item; item = item->next) {
            if (item->start < at || item->end > n || item->end < item->start || item->bits.before ||
                item->bits.after)
                error(&tok.loc,
                      "portable IR: overlapping or bitfield initializer is not implemented");
            if (item->start > at)
                datafield(&types, &values, text("[%llu x i16]", item->start - at), "[]");
            length = item->end - item->start;
            if (item->expr->kind == EXPRSTRING) {
                const uint_least16_t *s = item->expr->u.string.data;
                append(&chars, "[");
                for (i = 0; i < length; i++)
                    append(&chars, text("%s%u", i ? ", " : "",
                                        i < item->expr->u.string.size ? (unsigned)s[i] : 0));
                append(&chars, "]");
                arrayaddbuf(&chars, "", 1);
                datafield(&types, &values, text("[%llu x i16]", length), chars.val);
                chars.len = 0;
            } else {
                if (aggregate(item->expr->type) || length != item->expr->type->size)
                    error(&tok.loc, "portable IR: aggregate constant copy is not implemented");
                datafield(&types, &values, irtype(item->expr->type), constantvalue(item->expr));
            }
            at = item->end;
        }
        if (at < n)
            datafield(&types, &values, text("[%llu x i16]", n - at), "[]");
    }
    printf("  @\"%s\" = %s ", symbol(d->value), d->linkage == LINKEXTERN ? "external" : "internal");
    if (types.len) {
        fputs("struct {", stdout);
        fwrite(types.val, 1, types.len, stdout);
        fputs("} = {", stdout);
        fwrite(values.val, 1, values.len, stdout);
        putchar('}');
    } else {
        fputs(irtype(d->type), stdout);
        if (literal)
            printf(" = %s", literal);
    }
    putchar('\n');
    free(types.val);
    free(values.val);
    free(chars.val);
}

void rcc_finish(void)
{
    struct global *g;
    for (g = globals; g; g = g->next)
        if (!g->decl->defined) {
            if (g->decl->kind == DECLFUNC) {
                signature(g->decl, true);
                puts("  }");
            } else
                printf("  @\"%s\" = declaration %s\n", symbol(g->decl->value),
                       irtype(g->decl->type));
        }
}
