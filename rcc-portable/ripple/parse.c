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
static struct vec temporary_values;

struct interned {
    struct interned *next;
    char *text;
};
static struct interned *strings[1024];

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
    static struct type scalars[6];
    struct type *t;
    if (take("void"))
        t = &scalars[0];
    else if (take("[")) {
        unsigned long long n = integer();
        t = allocate(sizeof(*t));
        t->kind = TYARRAY;
        expect("x");
        t->base = type();
        expect("]");
        if (n > 0xffffffffUL || (t->base->size && n > 0xffffffffUL / t->base->size))
            fail("type is too large");
        t->size = (unsigned long)n * t->base->size;
    } else if (take("struct")) {
        t = allocate(sizeof(*t));
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
    } else {
        if (!strcmp(token, "i1") || !strcmp(token, "i8") || !strcmp(token, "i16")) {
            t = &scalars[1];
            t->kind = TYINT;
            t->size = 1;
        } else if (!strcmp(token, "i32") || !strcmp(token, "i64")) {
            t = &scalars[!strcmp(token, "i32") ? 2 : 3];
            t->kind = TYINT;
            t->size = !strcmp(token, "i32") ? 2 : 4;
        } else if (!strcmp(token, "f32") || !strcmp(token, "f64")) {
            t = &scalars[!strcmp(token, "f32") ? 4 : 5];
            t->kind = TYFLOAT;
            t->size = !strcmp(token, "f32") ? 2 : 4;
        } else
            fail("unsupported IR type '%s'", token);
        next();
    }
    while (take("*")) {
        struct type *p = allocate(sizeof(*p));
        p->kind = TYPOINTER;
        p->size = 2;
        p->base = t;
        t = p;
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
        struct value **cached;
        while (temporary_values.count <= id)
            append(&temporary_values, sizeof(v));
        cached = temporary_values.data;
        if (cached[id])
            return cached[id];
        v = allocate(sizeof(*v));
        v->kind = VTEMP;
        v->id = id;
        cached[id] = v;
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

struct module *parse(FILE *file, const char *unit)
{
    struct symbol *s;
    char *n;
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
            if (strcmp(token, "ripple"))
                fail("portable Ripple backend cannot compile target '%s'", token);
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
