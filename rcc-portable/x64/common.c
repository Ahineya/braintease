#include "x64.h"
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

struct section x64_sections[NSECTIONS] = {
    {".text", {0}, 16, 0}, {".data", {0}, 8, 0}, {".bss", {0}, 8, 0}, {".rodata", {0}, 8, 0}};
struct vector x64_symbols, x64_relocations;
unsigned x64_current_section;
const char *x64_source_path;
U32 x64_source_line;

void x64_error(const char *format, ...)
{
    va_list ap;
    fprintf(stderr, "%s:%lu: ", x64_source_path, x64_source_line);
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void *x64_memory(size_t size)
{
    void *p = calloc(size ? size : 1, 1);
    if (!p)
        x64_error("out of memory");
    return p;
}

void *x64_append(struct vector *v, size_t size)
{
    void *p;
    if (v->count == v->capacity) {
        size_t next = v->capacity ? v->capacity * 2 : 16;
        if (next < v->capacity || next > (size_t)-1 / size)
            x64_error("allocation overflow");
        p = realloc(v->data, next * size);
        if (!p)
            x64_error("out of memory");
        v->data = p;
        v->capacity = next;
    }
    p = (char *)v->data + v->count++ * size;
    memset(p, 0, size);
    return p;
}

char *x64_copy(const char *s)
{
    char *p = x64_memory(strlen(s) + 1);
    strcpy(p, s);
    return p;
}

char *x64_trim(char *s)
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

void x64_byte(struct buffer *b, unsigned value)
{
    if (b->size == b->capacity) {
        size_t next = b->capacity ? b->capacity * 2 : 256;
        void *p;
        if (next < b->capacity || next > 16UL * 1024 * 1024)
            x64_error("section exceeds 16 MiB");
        p = realloc(b->data, next);
        if (!p)
            x64_error("out of memory");
        b->data = p;
        b->capacity = next;
    }
    b->data[b->size++] = value & 255;
}

void x64_number(struct buffer *b, U64 value, unsigned width)
{
    unsigned i;
    for (i = 0; i < width; i++) {
        x64_byte(b, (unsigned)value);
        value >>= 8;
    }
}

void x64_emit(unsigned value)
{
    if (x64_current_section >= NSECTIONS)
        x64_error("no allocatable section selected");
    struct section *s = &x64_sections[x64_current_section];
    if (s->size == 16UL * 1024 * 1024)
        x64_error("section exceeds 16 MiB");
    if (x64_current_section == BSS) {
        if (value)
            x64_error("nonzero data in .bss");
    } else
        x64_byte(&s->bytes, value);
    s->size++;
}

void x64_emit_number(U64 value, unsigned width)
{
    unsigned i;
    for (i = 0; i < width; i++) {
        x64_emit((unsigned)value & 255);
        value >>= 8;
    }
}

void x64_emit_value(struct x64_expression value, unsigned width, unsigned type)
{
    if (value.symbol < 0 && width < 8) {
        unsigned bits = width * 8;
        U64 mask = ((U64)1 << bits) - 1;
        if (value.addend > mask && value.addend < ~(((U64)1 << (bits - 1)) - 1))
            x64_error("value exceeds encoded width");
    }
    if (value.symbol >= 0) {
        struct relocation *r;
        if (x64_current_section == BSS)
            x64_error("relocation in .bss");
        r = x64_append(&x64_relocations, sizeof(*r));
        r->section = x64_current_section;
        r->offset = x64_sections[x64_current_section].size;
        r->value = value;
        r->type = type;
        x64_emit_number(0, width);
    } else
        x64_emit_number(value.addend, width);
}

long x64_symbol_id(const char *name)
{
    size_t i;
    struct symbol *s = x64_symbols.data;
    if (!*name)
        x64_error("empty symbol");
    for (i = 0; i < x64_symbols.count; i++)
        if (!strcmp(s[i].name, name))
            return (long)i;
    s = x64_append(&x64_symbols, sizeof(*s));
    s->name = x64_copy(name);
    return (long)x64_symbols.count - 1;
}

/* One symbol plus an integral addend. Shifts are allowed only on constants.
 * C unsigned arithmetic deliberately implements two's-complement bit patterns. */
struct x64_expression x64_expression(char *text)
{
    struct x64_expression result = {-1, 0};
    char *s = x64_trim(text), *end;
    int sign = 1, first = 1;
    while (*s) {
        U64 value;
        while (isspace((unsigned char)*s))
            s++;
        if (*s == '+' || *s == '-') {
            sign = 1;
            do {
                if (*s++ == '-')
                    sign = -sign;
                while (isspace((unsigned char)*s))
                    s++;
            } while (*s == '+' || *s == '-');
        } else if (!first)
            x64_error("unsupported expression '%s'", text);
        while (isspace((unsigned char)*s))
            s++;
        if (isdigit((unsigned char)*s)) {
            errno = 0;
            value = strtoull(s, &end, 0);
            if (end == s || errno == ERANGE)
                x64_error("invalid or overflowing number");
            s = end;
            result.addend += sign < 0 ? 0 - value : value;
        } else {
            char saved;
            end = s;
            if (*s == '"') {
                s++;
                end = s;
                while (*end && *end != '"' && *end != '\\')
                    end++;
                if (*end != '"')
                    x64_error("invalid quoted symbol");
            } else
                while (isalnum((unsigned char)*end) || (*end && strchr("_.$", *end)))
                    end++;
            if (end == s || sign < 0 || result.symbol >= 0)
                x64_error("unsupported symbolic expression '%s'", text);
            saved = *end;
            *end = 0;
            result.symbol = x64_symbol_id(s);
            *end = saved;
            s = end;
            if (saved == '"')
                s++;
        }
        first = 0;
        while (isspace((unsigned char)*s))
            s++;
        if (s[0] == '>' && s[1] == '>') {
            unsigned long shift;
            if (result.symbol >= 0)
                x64_error("symbol shift is unsupported");
            shift = strtoul(s + 2, &end, 0);
            if (end == s + 2 || shift >= 64)
                x64_error("invalid expression shift");
            result.addend >>= shift;
            s = end;
        }
    }
    if (first)
        x64_error("missing expression");
    return result;
}
