/* RCC text IR reader for the portable Ripple backend. */
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "ripple.h"
const char *filename;
unsigned long line_number = 1;
static FILE *input;
static int ch;
static char *token;
/* Names retained by the module are copied before advancing the lexer. */
static struct vec token_text;
static unsigned long hash;
static struct module *module;

/* One compilation owns all IR nodes; small allocations share zeroed slabs. */
union arena_cell {
    void *pointer;
    unsigned long long bits;
    double number;
};
static union arena_cell *arena;
static size_t arena_left;

/* SSA IDs are labels, not allocation sizes. A sparse ID such as %1000000
 * must not append a million entries or force a multi-megabyte guest array. */
struct temporary_value {
    unsigned long id;
    struct value *value;
    struct temporary_value *next;
};
static struct temporary_value *temporary_values[1024];

struct interned {
    struct interned *next;
    char *text;
};
static struct interned *strings[1024];

/* Parsed types are immutable. Text IR repeats complete aggregate types at
 * every load/GEP/cast, so keeping a new tree per occurrence can dwarf the
 * actual instructions. Children are canonical before their parent is read. */
struct interned_type {
    struct interned_type *next;
    struct type type;
};
static struct interned_type *types[4096];

static unsigned type_hash(unsigned hash, struct type *t)
{
    uintptr_t p = (uintptr_t)t;
    return ((hash * 33) ^ (unsigned)(p >> 3) ^ (unsigned)(p >> 13)) & 4095;
}

static struct type *intern_type(struct type *t)
{
    struct interned_type *entry;
    struct type **fields = t->fields.data;
    size_t i;
    unsigned h = type_hash((unsigned)t->kind ^ (unsigned)t->size, t->base);
    for (i = 0; i < t->fields.count; i++)
        h = type_hash(h, fields[i]);
    for (entry = types[h]; entry; entry = entry->next) {
        struct type *p = &entry->type;
        if (p->kind != t->kind || p->size != t->size || p->bits != t->bits || p->base != t->base ||
            p->fields.count != t->fields.count)
            continue;
        for (i = 0; i < t->fields.count; i++)
            if (((struct type **)p->fields.data)[i] != fields[i])
                break;
        if (i == t->fields.count) {
            free(t->fields.data);
            return p;
        }
    }
    entry = allocate(sizeof(*entry));
    entry->type = *t;
    entry->next = types[h];
    types[h] = entry;
    return &entry->type;
}

void fail(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s:%lu: ", filename, line_number);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void *allocate(size_t n)
{
    void *p;
    size_t count = n / sizeof(*arena) + (n % sizeof(*arena) != 0);
    if (!count)
        count = 1;
    if (count > arena_left) {
        size_t capacity = count > 4096 ? count : 4096;
        arena = calloc(capacity, sizeof(*arena));
        if (!arena)
            fail("out of memory");
        arena_left = capacity;
    }
    p = arena;
    arena += count;
    arena_left -= count;
    return p;
}

char *duplicate(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned bucket = 5381;
    struct interned *entry;
    while (*p)
        bucket = bucket * 33 + *p++;
    bucket &= 1023;
    for (entry = strings[bucket]; entry; entry = entry->next)
        if (!strcmp(entry->text, s))
            return entry->text;
    entry = allocate(sizeof(*entry));
    entry->text = allocate(strlen(s) + 1);
    strcpy(entry->text, s);
    entry->next = strings[bucket];
    strings[bucket] = entry;
    return entry->text;
}

void *append(struct vec *v, size_t size)
{
    void *p;
    if (v->count == v->capacity) {
        size_t cap = v->capacity ? v->capacity * 2 : 8;
        if (cap < v->capacity || cap > ((size_t)-1) / size)
            fail("IR allocation overflow");
        p = realloc(v->data, cap * size);
        if (!p)
            fail("out of memory");
        v->data = p;
        v->capacity = cap;
    }
    p = (char *)v->data + v->count++ * size;
    memset(p, 0, size);
    return p;
}

static void readchar(void)
{
    if (ch == '\n')
        line_number++;
    ch = getc(input);
    if (ch != EOF)
        hash = ((hash ^ (unsigned)ch) * 16777619UL) & 0xffffffffUL;
}

static void next(void)
{
    struct vec *text = &token_text;
    char *p;
    int quote;
    text->count = 0;
    while (ch != EOF && isspace(ch))
        readchar();
    if (ch == ';') {
        while (ch != EOF && ch != '\n')
            readchar();
        next();
        return;
    }
    if (ch == EOF) {
        token = "";
        return;
    }
    quote = ch == '"';
    if (quote) {
        readchar();
        while (ch != EOF && ch != '"') {
            if (ch == '\\') {
                readchar();
                if (ch != '"' && ch != '\\')
                    fail("unsupported name escape");
            }
            p = append(text, 1);
            *p = (char)ch;
            readchar();
        }
        if (ch != '"')
            fail("unterminated quoted name");
        readchar();
    } else if (ch == '-') {
        p = append(text, 1);
        *p = '-';
        readchar();
        if (ch == '>') {
            p = append(text, 1);
            *p = '>';
            readchar();
        } else
            while (isdigit(ch)) {
                p = append(text, 1);
                *p = (char)ch;
                readchar();
            }
    } else if (isalnum(ch) || ch == '_' || ch == '.') {
        do {
            p = append(text, 1);
            *p = (char)ch;
            readchar();
        } while (isalnum(ch) || ch == '_' || ch == '.' || ch == '-');
    } else {
        p = append(text, 1);
        *p = (char)ch;
        readchar();
    }
    p = append(text, 1);
    *p = 0;
    token = text->data;
}

static bool take(const char *s)
{
    if (strcmp(token, s))
        return false;
    next();
    return true;
}

static void expect(const char *s)
{
    if (!take(s))
        fail("expected '%s', got '%s'", s, token);
}

static unsigned long long integer(void)
{
    unsigned long long n;
    char *end;
    errno = 0;
    n = strtoull(token, &end, 10);
    if (!*token || *end || errno == ERANGE)
        fail("invalid integer '%s'", token);
    next();
    return n;
}

static unsigned long index_value(void)
{
    unsigned long long n = integer();
    if (n > 1000000UL)
        fail("IR index exceeds 1000000");
    return (unsigned long)n;
}

static char *name(void)
{
    char *s;
    expect("@");
    if (!*token)
        fail("expected symbol name");
    s = duplicate(token);
    next();
    return s;
}

struct symbol *symbol(struct module *m, const char *name)
{
    size_t i;
    struct symbol *s = m->symbols.data;
    for (i = 0; i < m->symbols.count; i++)
        if (!strcmp(s[i].name, name))
            return &s[i];
    s = append(&m->symbols, sizeof(*s));
    s->name = duplicate(name);
    return s;
}

static struct type *type(void)
{
    /* Scalar types are immutable. Reusing them avoids a heap allocation for
     * almost every instruction in a compiler-sized translation unit. */
    static struct type scalars[8];
    struct type candidate = {0};
    struct type *t;
    if (take("void"))
        t = &scalars[0];
    else if (take("[")) {
        unsigned long long n = integer();
        t = &candidate;
        t->kind = TYARRAY;
        expect("x");
        t->base = type();
        expect("]");
        if (n > 0xffffffffUL || (t->base->size && n > 0xffffffffUL / t->base->size))
            fail("type is too large");
        t->size = (unsigned long)n * t->base->size;
        t = intern_type(t);
    } else if (take("struct")) {
        t = &candidate;
        t->kind = TYSTRUCT;
        if (strcmp(token, "{"))
            next();
        expect("{");
        while (!take("}")) {
            struct type *field = type();
            *(struct type **)append(&t->fields, sizeof(field)) = field;
            if (field->size > 0xffffffffUL - t->size)
                fail("struct size overflow");
            t->size += field->size;
            if (strcmp(token, "}"))
                expect(",");
        }
        t = intern_type(t);
    } else {
        if (!strcmp(token, "i1") || !strcmp(token, "i8") || !strcmp(token, "i16")) {
            unsigned at = !strcmp(token, "i1") ? 6 : !strcmp(token, "i8") ? 7 : 1;
            t = &scalars[at];
            t->kind = TYINT;
            t->size = 1;
            t->bits = at == 6 ? 1 : at == 7 ? 8 : 16;
        } else if (!strcmp(token, "i32") || !strcmp(token, "i64")) {
            t = &scalars[!strcmp(token, "i32") ? 2 : 3];
            t->kind = TYINT;
            t->size = !strcmp(token, "i32") ? 2 : 4;
            t->bits = (unsigned)t->size * 16;
        } else if (!strcmp(token, "f32") || !strcmp(token, "f64")) {
            t = &scalars[!strcmp(token, "f32") ? 4 : 5];
            t->kind = TYFLOAT;
            t->size = !strcmp(token, "f32") ? 2 : 4;
            t->bits = (unsigned)t->size * 16;
        } else
            fail("unsupported IR type '%s'", token);
        next();
    }
    while (take("*")) {
        struct type pointer = {0};
        pointer.kind = TYPOINTER;
        pointer.size = 2;
        pointer.bits = 32;
        pointer.base = t;
        t = intern_type(&pointer);
    }
    return t;
}

unsigned int slots(struct type *t)
{
    return t->kind == TYARRAY || t->kind == TYSTRUCT ? 2 : (unsigned int)t->size;
}

static struct value *value(void)
{
    struct value *v;
    if (take("%")) {
        unsigned long id = index_value();
        unsigned bucket = (unsigned)(id ^ (id >> 10)) & 1023;
        struct temporary_value *cached;
        for (cached = temporary_values[bucket]; cached; cached = cached->next)
            if (cached->id == id)
                return cached->value;
        v = allocate(sizeof(*v));
        v->kind = VTEMP;
        v->id = id;
        cached = allocate(sizeof(*cached));
        cached->id = id;
        cached->value = v;
        cached->next = temporary_values[bucket];
        temporary_values[bucket] = cached;
        return v;
    }
    v = allocate(sizeof(*v));
    if (take("global")) {
        v->kind = VGLOBAL;
        v->name = name();
        symbol(module, v->name);
    } else if (take("func")) {
        v->kind = VFUNCTION;
        v->name = name();
        symbol(module, v->name)->function = true;
    } else if (take("fat")) {
        v->kind = VFAT;
        expect("(");
        v->base = value();
        expect(",");
        if (take("global"))
            v->bank = BGLOBAL;
        else if (take("stack"))
            v->bank = BSTACK;
        else if (take("mixed") || take("unknown"))
            v->bank = BMIXED;
        else if (take("null"))
            v->bank = BNULL;
        else if (take("heap")) {
            expect("(");
            v->bank = (int)(unsigned int)integer();
            expect(")");
        } else
            fail("invalid bank tag '%s'", token);
        expect(")");
    } else if (take("offset")) {
        v->kind = VOFFSET;
        expect("(");
        v->base = value();
        expect(",");
        v->offset = (long)integer();
        expect(")");
    } else if (!strcmp(token, "[") || !strcmp(token, "{")) {
        const char *close = take("[") ? "]" : "}";
        if (*close == '}')
            expect("{");
        v->kind = *close == ']' ? VARRAY : VAGGREGATE;
        while (!take(close)) {
            *(struct value **)append(&v->items, sizeof(v)) = value();
            if (strcmp(token, close))
                expect(",");
        }
    } else if (take("undef"))
        v->kind = VUNDEF;
    else {
        v->kind = VNUMBER;
        v->bits = integer();
    }
    return v;
}

static void arguments(struct instruction *inst)
{
    expect("(");
    while (!take(")")) {
        *(struct value **)append(&inst->args, sizeof(struct value *)) = value();
        if (strcmp(token, ")"))
            expect(",");
    }
}

static struct instruction *instruction(void)
{
    struct instruction *i = allocate(sizeof(*i));
    i->result = -1;
    i->stack_from = 0xffffffffUL;
    i->line = line_number;
    if (take("%")) {
        i->result = (long)index_value();
        expect("=");
    }
    i->op = duplicate(token);
    next();
    if (!strcmp(i->op, "ret")) {
        if (!take("void"))
            i->a = value();
    } else if (!strcmp(i->op, "br")) {
        if (!take("label")) {
            i->conditional = true;
            i->a = value();
            expect(",");
            expect("label");
        }
        i->yes = index_value();
        if (i->conditional) {
            expect(",");
            expect("label");
            i->no = index_value();
        }
    } else if (!strcmp(i->op, "call") || !strcmp(i->op, "intrinsic")) {
        i->type = type();
        i->a = !strcmp(i->op, "intrinsic") ? allocate(sizeof(*i->a)) : value();
        if (!strcmp(i->op, "intrinsic")) {
            i->a->kind = VFUNCTION;
            i->a->name = name();
        }
        arguments(i);
        if (take("stack"))
            i->stack_from = index_value();
    } else if (!strcmp(i->op, "alloca")) {
        i->source_type = type();
        if (take(","))
            i->a = value();
        expect("->");
        i->type = type();
    } else if (!strcmp(i->op, "getelementptr")) {
        i->type = type();
        expect(",");
        i->source_type = type();
        i->a = value();
        while (take(","))
            *(struct value **)append(&i->args, sizeof(struct value *)) = value();
        if (take("field"))
            fail("struct GEP indices are not implemented; use frontend cell offsets");
    } else if (!strcmp(i->op, "phi")) {
        i->type = type();
        do {
            expect("[");
            *(struct value **)append(&i->args, sizeof(struct value *)) = value();
            expect(",");
            expect("label");
            *(unsigned long *)append(&i->labels, sizeof(unsigned long)) = index_value();
            expect("]");
        } while (take(","));
    } else if (!strcmp(i->op, "load") || !strcmp(i->op, "store")) {
        take("volatile");
        i->type = type();
        if (!strcmp(i->op, "store"))
            i->a = value();
        expect(",");
        i->b = value();
    } else if (!strcmp(i->op, "select")) {
        i->type = type();
        i->a = value();
        expect(",");
        i->b = value();
        expect(",");
        i->c = value();
    } else {
        size_t j;
        bool binary = false;
        i->type = type();
        i->a = value();
        for (j = 0; j < 24; j++)
            if (!strcmp(i->op, binary_ops[j]))
                binary = true;
        if (binary) {
            expect(",");
            i->b = value();
        } else if (strcmp(i->op, "neg") && strcmp(i->op, "not") && strcmp(i->op, "zext") &&
                   strcmp(i->op, "sext") && strcmp(i->op, "trunc") && strcmp(i->op, "ptrtoint") &&
                   strcmp(i->op, "ptrtolinear") && strcmp(i->op, "inttoptr"))
            fail("unsupported IR operation '%s'", i->op);
    }
    return i;
}

static void function(void)
{
    struct function *f = append(&module->functions, sizeof(*f));
    struct symbol *s;
    f->serial = module->functions.count;
    f->external = take("external");
    f->vararg = take("vararg");
    f->result = type();
    f->name = name();
    s = symbol(module, f->name);
    s->function = true;
    expect("(");
    while (!take(")")) {
        struct parameter p;
        p.type = type();
        expect("%");
        p.id = index_value();
        *(struct parameter *)append(&f->parameters, sizeof(p)) = p;
        if (strcmp(token, ")"))
            expect(",");
    }
    expect("{");
    while (!take("}")) {
        struct block *b = append(&f->blocks, sizeof(*b));
        expect("block");
        b->id = index_value();
        while (strcmp(token, ":")) {
            if (!*token)
                fail("unterminated block header");
            next();
        }
        expect(":");
        while (strcmp(token, "block") && strcmp(token, "}"))
            *(struct instruction **)append(&b->instructions, sizeof(struct instruction *)) =
                instruction();
    }
    if (f->external && f->blocks.count)
        fail("external function has a body");
    if (!f->external) {
        s = symbol(module, f->name);
        if (s->defined)
            fail("duplicate function '%s'", f->name);
        s->defined = true;
    }
}

struct module *parse_target(FILE *file, const char *unit, const char *target)
{
    struct symbol *s;
    char *n;
    bool target_seen = false;
    input = file;
    hash = 2166136261UL;
    ch = 0;
    readchar();
    next();
    module = allocate(sizeof(*module));
    expect("module");
    if (!*token)
        fail("expected module name");
    next();
    expect("{");
    while (!take("}")) {
        if (take("target")) {
            if (target_seen)
                fail("duplicate IR target");
            target_seen = true;
            if (strcmp(token, target))
                fail("portable %s backend cannot compile target '%s'", target, token);
            next();
        } else if (take("function"))
            function();
        else if (take("internal")) {
            expect("function");
            n = name();
            symbol(module, n)->internal = true;
        } else if (!strcmp(token, "@")) {
            bool declaration = false, internal = false;
            struct type *t;
            struct value *init = NULL;
            n = name();
            expect("=");
            if (take("declaration"))
                declaration = true;
            else if (take("internal") || take("private"))
                internal = true;
            else
                expect("external");
            take("constant");
            t = type();
            if (take("="))
                init = value();
            s = symbol(module, n);
            s->internal |= internal;
            if (!s->type)
                s->type = t;
            if (!declaration) {
                if (s->defined)
                    fail("duplicate global '%s'", n);
                s->defined = true;
                s->type = t;
                s->init = init;
            }
        } else
            fail("unsupported module item '%s'", token);
    }
    if (*token)
        fail("trailing IR input '%s'", token);
    if (!target_seen && strcmp(target, "ripple"))
        fail("portable %s backend requires an explicit IR target", target);
    {
        unsigned long identity = 2166136261UL;
        const unsigned char *p = (const unsigned char *)unit;
        for (; *p; p++)
            identity = ((identity ^ *p) * 16777619UL) & 0xffffffffUL;
        snprintf(module->prefix, sizeof(module->prefix), "__rcp_%08lx_%08lx", hash, identity);
    }
    {
        size_t i, j;
        struct symbol *symbols = module->symbols.data;
        for (i = 0; i < module->symbols.count; i++) {
            s = &symbols[i];
            for (j = 0; s->name[j]; j++)
                if (!isalnum((unsigned char)s->name[j]) && s->name[j] != '_')
                    fail("symbol '%s' is not an assembler identifier", s->name);
            if (!*s->name)
                fail("empty symbol name");
            if (s->internal) {
                s->label = allocate(strlen(s->name) + 48);
                sprintf(s->label, "%s_%s", module->prefix, s->name);
            } else
                s->label = s->name;
        }
    }
    return module;
}

struct module *parse(FILE *file, const char *unit)
{
    return parse_target(file, unit, "ripple");
}
