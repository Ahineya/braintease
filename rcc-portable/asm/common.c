#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "tools.h"
const char *source_file = "rcc-tools";
unsigned long source_line;
static char *temporary, *destination;

static void cleanup(void)
{
    if (temporary)
        unlink(temporary);
}

void fatal(const char *format, ...)
{
    va_list args;
    fprintf(stderr, "%s:%lu: ", source_file, source_line);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    exit(1);
}

void *allocate_zeroed(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p)
        fatal("out of memory");
    return p;
}

char *copy_string(const char *s)
{
    char *p = allocate_zeroed(strlen(s) + 1);
    strcpy(p, s);
    return p;
}

void *array_append(struct array *a, size_t size)
{
    void *p;
    if (a->count == a->capacity) {
        size_t capacity = a->capacity ? a->capacity * 2 : 8;
        if (capacity < a->capacity || capacity > ((size_t)-1) / size)
            fatal("allocation overflow");
        p = realloc(a->data, capacity * size);
        if (!p)
            fatal("out of memory");
        a->data = p;
        a->capacity = capacity;
    }
    p = (char *)a->data + a->count++ * size;
    memset(p, 0, size);
    return p;
}

void put16(FILE *f, unsigned n)
{
    putc(n & 255, f);
    putc((n >> 8) & 255, f);
}

void put32(FILE *f, unsigned long n)
{
    put16(f, (unsigned)n);
    put16(f, (unsigned)(n >> 16));
}

unsigned get16(FILE *f)
{
    int a = getc(f), b = getc(f);
    if (a == EOF || b == EOF)
        fatal("truncated object");
    return (unsigned)a | ((unsigned)b << 8);
}

unsigned long get32(FILE *f)
{
    unsigned long a = get16(f), b = get16(f);
    return a | (b << 16);
}

void putstr(FILE *f, const char *s)
{
    size_t n = strlen(s);
    put32(f, (unsigned long)n);
    while (*s)
        putc((unsigned char)*s++, f);
}

char *getstr(FILE *f)
{
    unsigned long n = get32(f), i;
    char *s;
    if (n > 8192)
        fatal("object name exceeds 8192 characters");
    s = allocate_zeroed((size_t)n + 1);
    for (i = 0; i < n; i++) {
        int ch = getc(f);
        if (ch == EOF || !ch)
            fatal("invalid object name");
        s[i] = (char)ch;
    }
    return s;
}

FILE *open_input(const char *name)
{
    FILE *f = fopen(name, "rb");
    if (!f)
        fatal("cannot open '%s'", name);
    return f;
}

FILE *open_output(const char *name)
{
    FILE *f;
    if (temporary)
        fatal("output already open");
    destination = copy_string(name);
    temporary = allocate_zeroed(strlen(name) + 40);
    sprintf(temporary, "%s.tmp.%u", name, (unsigned)getpid());
    atexit(cleanup);
    f = fopen(temporary, "wb");
    if (!f)
        fatal("cannot create output '%s'", name);
    return f;
}

void finish_output(FILE *f)
{
    if (fflush(f) || ferror(f) || fclose(f))
        fatal("cannot write output");
    if (rename(temporary, destination))
        fatal("cannot publish output '%s'", destination);
    free(temporary);
    temporary = NULL;
}

void magic(FILE *f, const char *s)
{
    while (*s)
        if (getc(f) != (unsigned char)*s++)
            fatal("invalid object/archive format");
}

void put_record(FILE *f, struct record *r)
{
    put16(f, r->op);
    put16(f, r->a);
    put16(f, r->b);
    put16(f, r->c);
    put16(f, r->kind);
    put16(f, r->target);
    put32(f, r->value);
    put32(f, (unsigned long)r->addend);
}

void get_record(FILE *f, struct record *r)
{
    r->op = get16(f);
    r->a = get16(f);
    r->b = get16(f);
    r->c = get16(f);
    r->kind = get16(f);
    r->target = get16(f);
    r->value = get32(f);
    {
        unsigned long v = get32(f);
        r->addend = v & 0x80000000UL ? -(long)(0xffffffffUL - v) - 1 : (long)v;
    }
    if ((r->op > OP_ENTERU && r->op != OP_NOP) || r->kind > RDATA || r->target > TEXTERNAL)
        fatal("invalid instruction record");
}

/* Assembly can contain millions of short lines. Keep one buffer per scan,
 * rather than allocating and freeing several buffers for every instruction. */
char *readline(FILE *f, struct array *buffer)
{
    int ch;
    bool comment = false;
    char *text;
    buffer->count = 0;
    if (!buffer->capacity) {
        buffer->capacity = 256;
        buffer->data = allocate_zeroed(buffer->capacity);
    }
    text = buffer->data;
    while ((ch = getc(f)) != EOF && ch != '\n') {
        if (ch == ';')
            comment = true;
        if (comment)
            continue;
        if (buffer->count == 65535UL)
            fatal("assembly line exceeds 65535 characters");
        if (buffer->count + 1 == buffer->capacity) {
            size_t capacity = buffer->capacity * 2;
            text = realloc(buffer->data, capacity);
            if (!text)
                fatal("out of memory reading assembly");
            buffer->data = text;
            buffer->capacity = capacity;
        }
        text[buffer->count++] = (char)ch;
    }
    if (ch == EOF && !buffer->count)
        return NULL;
    text[buffer->count] = 0;
    source_line++;
    return text;
}

char *trim(char *s)
{
    char *end;
    while (isspace((unsigned char)*s))
        s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        --end;
    *end = 0;
    return s;
}

struct object *read_object(FILE *f, const char *path)
{
    struct object *o = allocate_zeroed(sizeof(*o));
    unsigned long ns, ne, i, end;
    o->path = path;
    o->body = (unsigned long)ftell(f);
    magic(f, "RPO1");
    if (get32(f) != 1)
        fatal("unsupported object version");
    o->ncode = get32(f);
    o->ndata = get32(f);
    ns = get32(f);
    ne = get32(f);
    o->banked = get32(f) != 0;
    if (o->ncode > 16000000UL || o->ncode % CHUNK || o->ndata >= 16UL * BANK || ns > 100000 ||
        ne > 100000)
        fatal("invalid object dimensions");
    for (i = 0; i < ns; i++) {
        struct definition *d = array_append(&o->defs, sizeof(*d));
        d->name = getstr(f);
        d->kind = get16(f);
        d->value = get32(f);
        if (d->kind > 1 || d->value > (d->kind ? o->ndata : o->ncode))
            fatal("object symbol outside section");
    }
    for (i = 0; i < ne; i++)
        *(char **)array_append(&o->externs, sizeof(char *)) = getstr(f);
    o->code_at = (unsigned long)ftell(f);
    if (o->ncode > (0x7fffffffUL - o->code_at) / RECORD)
        fatal("object file too large");
    o->data_at = o->code_at + o->ncode * RECORD;
    end = o->data_at + o->ndata;
    if (end < o->data_at || end > 0x7fffffffUL)
        fatal("object file too large");
    if (fseek(f, 0, SEEK_END) || (unsigned long)ftell(f) < end || fseek(f, (long)end, SEEK_SET))
        fatal("truncated object payload");
    return o;
}
