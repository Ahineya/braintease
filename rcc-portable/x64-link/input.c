#include "link.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

const char *ld_path = "rcc-x64-ld";
struct ld_vector ld_objects, ld_functions, ld_fixups;
L32 ld_entry;

void ld_error(const char *format, ...)
{
    va_list args;
    fprintf(stderr, "rcc-x64-ld: %s: ", ld_path);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    exit(1);
}

void *ld_memory(size_t size)
{
    void *p = calloc(size ? size : 1, 1);
    if (!p)
        ld_error("out of memory (allocation of %lu C bytes)", (unsigned long)size);
    return p;
}

void *ld_append(struct ld_vector *v, size_t size)
{
    void *p;
    if (v->count == v->capacity) {
        size_t cap = v->capacity ? v->capacity * 2 : 32;
        if (cap > LD_LIMIT / size)
            ld_error("table capacity exceeded");
        p = realloc(v->data, cap * size);
        if (!p)
            ld_error("out of memory growing table (%lu entries of %lu C bytes)", (unsigned long)cap,
                     (unsigned long)size);
        v->data = p;
        v->capacity = cap;
    }
    p = (char *)v->data + v->count++ * size;
    memset(p, 0, size);
    return p;
}

L64 ld_read(const unsigned char *p, unsigned width)
{
    L64 n = 0;
    while (width)
        n = (n << 8) | p[--width];
    return n;
}

void ld_patch(unsigned char *p, L64 value, unsigned width)
{
    while (width--) {
        *p++ = value & 255;
        value >>= 8;
    }
}

static L32 bounded(L64 value)
{
    if (value > LD_LIMIT)
        ld_error("ELF quantity exceeds supported capacity");
    return (L32)value;
}

static void span(struct ld_object *o, L32 at, L32 size)
{
    if (at > o->size || size > o->size - at)
        ld_error("ELF table/section outside input");
}

static const char *string_at(struct ld_object *o, struct ld_section *s, L32 at)
{
    L32 i;
    if (s->type != 3 || at >= s->size)
        ld_error("invalid ELF string table/index");
    for (i = at; i < s->size; i++)
        if (!o->data[s->offset + i])
            return (const char *)o->data + s->offset + at;
    ld_error("unterminated ELF string");
    return NULL;
}

static int section_order(const void *a, const void *b)
{
    const struct ld_section *x = *(struct ld_section *const *)a;
    const struct ld_section *y = *(struct ld_section *const *)b;
    return x->offset < y->offset ? -1 : x->offset > y->offset;
}

/* Keep names and compiler metadata. Machine code is copied from
 * the input only after the final layout is known; retaining every input plus
 * the entire output image doubles the dominant allocation in large links. */
static void compact(struct ld_object *o, struct ld_section *names)
{
    struct ld_section **kept = ld_memory(o->section_count * sizeof *kept);
    unsigned char *data;
    L32 i, count = 0, begin = 0, end = 0, destination = 0, size = 0;
    names->resident = 1;
    for (i = 1; i < o->symbol_count; i++) {
        struct ld_symbol *s = &o->symbols[i];
        if (!strcmp(s->name, NM_SYMBOL) && s->section && s->section < o->section_count &&
            o->sections[s->section].type == 1)
            o->sections[s->section].resident = 1;
    }
    for (i = 0; i < o->section_count; i++) {
        struct ld_section *s = &o->sections[i];
        if (s->resident)
            kept[count++] = s;
        s->resident = LD_LIMIT + 1;
    }
    qsort(kept, count, sizeof *kept, section_order);
    for (i = 0; i < count; i++) {
        struct ld_section *s = kept[i];
        if (!i || s->offset > end) {
            begin = s->offset;
            end = begin;
            destination = size;
        }
        s->resident = destination + s->offset - begin;
        if (s->offset + s->size > end) {
            size += s->offset + s->size - end;
            end = s->offset + s->size;
        }
    }
    /* The union of validated file spans cannot exceed the original object. */
    data = ld_memory(size);
    for (i = 0; i < count; i++)
        memcpy(data + kept[i]->resident, o->data + kept[i]->offset, kept[i]->size);
    for (i = 0; i < o->symbol_count; i++)
        o->symbols[i].name = (const char *)data + names->resident +
                             (o->symbols[i].name - ((const char *)o->data + names->offset));
    free(kept);
    free(o->data);
    o->data = data;
}

const unsigned char *ld_section_data(struct ld_object *o, struct ld_section *s)
{
    if (s->resident > LD_LIMIT)
        ld_error("section is not resident");
    return o->data + s->resident;
}

static L32 checksum(L32 hash, const unsigned char *p, L32 size)
{
    L32 i;
    for (i = 0; i < size; i++) {
        if (p[i] > 255)
            ld_error("ELF input contains non-octet cells");
        hash = ((hash ^ p[i]) * 16777619UL) & 0xffffffffUL;
    }
    return hash;
}

static FILE *reopen_input(struct ld_object *o)
{
    FILE *f = fopen(o->source, "rb");
    long length;
    if (!f || fseek(f, 0, SEEK_END) || (length = ftell(f)) < 0 || (L32)length != o->source_size ||
        fseek(f, (long)o->start, SEEK_SET))
        ld_error("cannot reopen unchanged ELF input");
    return f;
}

unsigned char *ld_read_relocations(struct ld_object *o, struct ld_section *s)
{
    FILE *f = reopen_input(o);
    unsigned char *data = ld_memory(s->size);
    if (fseek(f, (long)(o->start + s->offset), SEEK_SET) || fread(data, 1, s->size, f) != s->size ||
        fclose(f))
        ld_error("cannot reread ELF relocations");
    if (checksum(2166136261UL, data, s->size) != s->checksum)
        ld_error("ELF relocations changed during link");
    return data;
}

void ld_copy_sections(struct ld_object *o)
{
    unsigned char buffer[4096];
    L32 at, i, hash = 2166136261UL;
    FILE *f = reopen_input(o);
    /* Catch replacement/truncation after cataloguing, including archive
     * members. One descriptor at a time also bounds guest descriptor usage. */
    for (at = 0; at < o->size;) {
        L32 n = o->size - at < sizeof buffer ? o->size - at : sizeof buffer;
        if (fread(buffer, 1, n, f) != n)
            ld_error("cannot reread ELF input");
        hash = checksum(hash, buffer, n);
        for (i = 1; i < o->section_count; i++) {
            struct ld_section *s = &o->sections[i];
            L32 begin = at > s->offset ? at : s->offset;
            L32 end = at + n < s->offset + s->size ? at + n : s->offset + s->size;
            if ((s->flags & 2) && s->type != 8 && begin < end)
                ld_image_copy(s->address + (begin - s->offset), buffer + (begin - at), end - begin);
        }
        at += n;
    }
    if (hash != o->checksum)
        ld_error("ELF input changed during link");
    if (fclose(f))
        ld_error("cannot close ELF input");
}

struct ld_object *ld_read_object(const char *path, L32 start, L32 size)
{
    struct ld_object *o = ld_memory(sizeof *o);
    struct ld_section *symtab = NULL;
    FILE *f;
    long length = 0;
    L32 i, shoff, names;
    ld_path = o->path = o->source = path;
    f = fopen(path, "rb");
    if (!f || fseek(f, 0, SEEK_END) || (length = ftell(f)) < 0 || start > (L32)length ||
        (size && size > (L32)length - start))
        ld_error("cannot read bounded ELF input");
    o->size = size ? size : (L32)length - start;
    o->start = start;
    o->source_size = (L32)length;
    if (o->size < 64 || o->size > LD_LIMIT || fseek(f, (long)start, SEEK_SET))
        ld_error("cannot read bounded ELF input");
    o->data = ld_memory(o->size);
    if (fread(o->data, 1, o->size, f) != o->size || fclose(f))
        ld_error("cannot finish reading object");
    o->checksum = checksum(2166136261UL, o->data, o->size);
    if (memcmp(o->data, "\177ELF\2\1\1", 7) || ld_read(o->data + 16, 2) != 1 ||
        ld_read(o->data + 18, 2) != 62 || ld_read(o->data + 20, 4) != 1 ||
        ld_read(o->data + 24, 8) || ld_read(o->data + 32, 8) || ld_read(o->data + 48, 4) ||
        ld_read(o->data + 52, 2) != 64 || ld_read(o->data + 56, 2) ||
        ld_read(o->data + 58, 2) != 64)
        ld_error("expected ELF64 little-endian x86_64 relocatable");
    shoff = bounded(ld_read(o->data + 40, 8));
    o->section_count = (L32)ld_read(o->data + 60, 2);
    names = (L32)ld_read(o->data + 62, 2);
    if (!o->section_count || o->section_count >= 65280 || names >= o->section_count)
        ld_error("extended/invalid ELF section table");
    span(o, shoff, o->section_count * 64);
    o->sections = ld_memory(o->section_count * sizeof *o->sections);
    for (i = 0; i < o->section_count; i++) {
        const unsigned char *p = o->data + shoff + i * 64;
        struct ld_section *s = &o->sections[i];
        if (ld_kernel_mode && ld_read(p + 4, 4) == 0x6fff4c03UL &&
            ld_read(p + 8, 8) == 0x80000000UL) {
            /* LLVM's excluded address-significance hints only guide optional
             * identical-code folding. This linker does not fold sections. */
            s->type = 1;
            s->flags = 0;
        } else {
            s->type = bounded(ld_read(p + 4, 4));
            s->flags = bounded(ld_read(p + 8, 8));
        }
        s->offset = bounded(ld_read(p + 24, 8));
        if (ld_kernel_mode && s->type == 8) {
            L64 size = ld_read(p + 32, 8);
            if (size > 128UL * 1024 * 1024)
                ld_error("kernel BSS exceeds 128 MiB");
            s->size = (L32)size;
        } else
            s->size = bounded(ld_read(p + 32, 8));
        s->link = bounded(ld_read(p + 40, 4));
        s->info = bounded(ld_read(p + 44, 4));
        s->alignment = bounded(ld_read(p + 48, 8));
        s->entsize = bounded(ld_read(p + 56, 8));
        if (ld_read(p + 16, 8) || s->alignment > 4096 ||
            (s->alignment && (s->alignment & (s->alignment - 1))))
            ld_error("unsupported ELF section address/alignment");
        if (s->type != 8)
            span(o, s->offset, s->size);
        if (s->flags & 2) {
            /* Preserve mergeable constants verbatim: deduplication is optional
             * and changing symbol offsets without rewriting relocs is not. */
            if ((s->flags & ~55UL) || (s->type != 1 && s->type != 8) ||
                ((s->flags & 48) && s->type != 1))
                ld_error("unsupported allocated section (TLS/groups are not supported)");
        } else if ((s->flags && !(s->type == 4 && s->flags == 64) &&
                    !(s->type == 1 && !(s->flags & ~48UL))) ||
                   (s->type != 0 && s->type != 1 && s->type != 2 && s->type != 3 && s->type != 4 &&
                    s->type != 8))
            ld_error("unsupported ELF section type/flags");
        if (!i && (s->type || s->flags || s->size || s->link || s->info))
            ld_error("invalid ELF null section");
        if (s->type == 2) {
            if (symtab)
                ld_error("multiple symbol tables are not supported");
            symtab = s;
        }
    }
    for (i = 0; i < o->section_count; i++) {
        const unsigned char *p = o->data + shoff + i * 64;
        string_at(o, &o->sections[names], bounded(ld_read(p, 4)));
    }
    if (!symtab || symtab->entsize != 24 || symtab->size % 24 || symtab->link >= o->section_count ||
        !symtab->size || symtab->info > symtab->size / 24)
        ld_error("invalid ELF symbol table");
    o->symbol_count = symtab->size / 24;
    o->symbols = ld_memory(o->symbol_count * sizeof *o->symbols);
    for (i = 0; i < o->symbol_count; i++) {
        const unsigned char *p = o->data + symtab->offset + i * 24;
        struct ld_symbol *s = &o->symbols[i];
        s->name = string_at(o, &o->sections[symtab->link], bounded(ld_read(p, 4)));
        s->binding = p[4] >> 4;
        s->type = p[4] & 15;
        s->section = (unsigned)ld_read(p + 6, 2);
        s->value = ld_read(p + 8, 8);
        s->size = ld_read(p + 16, 8);
        s->object = o;
        s->helper = -1;
        if (s->binding > 1 || s->type > 4 || (p[5] != 0 && p[5] != 2) ||
            (i < symtab->info ? s->binding != 0 : s->binding != 1))
            ld_error("unsupported symbol binding/type/visibility");
        if (s->section == 65521U) {
            /* SHN_ABS is a constant, not a relocatable native address. */
        } else if (s->section >= o->section_count)
            ld_error("invalid/unsupported symbol section (COMMON is not supported)");
        else if (s->section && (s->value > o->sections[s->section].size ||
                                s->size > o->sections[s->section].size - s->value))
            ld_error("symbol outside its section");
        if (!i && (*s->name || s->section || s->value || s->size || s->type || s->binding))
            ld_error("invalid ELF null symbol");
        if (i && !s->section && (!s->binding || !*s->name || s->value || s->size))
            ld_error("invalid undefined symbol");
        if (s->binding && !*s->name)
            ld_error("unnamed global symbol");
    }
    for (i = 0; i < o->section_count; i++) {
        struct ld_section *s = &o->sections[i];
        if (s->type == 4 &&
            (s->entsize != 24 || s->size % 24 || s->link != (L32)(symtab - o->sections) ||
             !s->info || s->info >= o->section_count || !(o->sections[s->info].flags & 2)))
            ld_error("invalid/unsupported RELA target/table");
        if (s->type == 4)
            s->checksum = checksum(2166136261UL, o->data + s->offset, s->size);
    }
    compact(o, &o->sections[symtab->link]);
    return o;
}

void ld_free_object(struct ld_object *o)
{
    free(o->data);
    free(o->sections);
    free(o->symbols);
    free(o);
}

void ld_object(const char *path)
{
    ld_include(ld_read_object(path, 0, 0));
}

/* This explicit --metadata input is the current lowerer's bootstrap
 * contract. It is never inferred from arbitrary assembly comments or symbols. */
static void space(char **p)
{
    while (isspace((unsigned char)**p))
        ++*p;
}

static L32 decimal(char **p, L32 max)
{
    L32 n = 0;
    space(p);
    if (!isdigit((unsigned char)**p))
        ld_error("expected unsigned metadata number");
    while (isdigit((unsigned char)**p)) {
        unsigned digit = (unsigned)(*(*p)++ - '0');
        if (n > max / 10 || (n == max / 10 && digit > max % 10))
            ld_error("metadata number exceeds capacity");
        n = n * 10 + digit;
    }
    if (**p && !isspace((unsigned char)**p))
        ld_error("invalid metadata number");
    return n;
}

void ld_metadata(const char *path)
{
    FILE *f;
    char line[1024], name[256], *p;
    L32 token, arity;
    size_t i, used;
    int c, overflow;
    ld_path = path;
    f = fopen(path, "r");
    if (!f)
        ld_error("cannot open continuation metadata");
    for (;;) {
        used = 0;
        overflow = 0;
        while ((c = getc(f)) != EOF && c != '\n') {
            if (!c || c > 255)
                ld_error("metadata input contains NUL/non-octet cells");
            if (used < sizeof line - 1)
                line[used++] = (char)c;
            else
                overflow = 1;
        }
        if (!used && c == EOF)
            break;
        line[used] = 0;
        if (strncmp(line, "# rcc-", 6))
            continue; /* Ordinary QBE lines may be arbitrarily long. */
        if (overflow)
            ld_error("metadata directive exceeds capacity");
        for (i = 0; i < used; i++)
            if ((unsigned char)line[i] > 127)
                ld_error("metadata directives must use ASCII");
        if (!strncmp(line, "# rcc-entry", 11)) {
            p = line + 11;
            if (!isspace((unsigned char)*p))
                ld_error("invalid entry metadata directive");
            token = decimal(&p, 8191);
            space(&p);
            if (ld_entry || !token || *p)
                ld_error("invalid/duplicate entry metadata");
            ld_entry = token;
        } else if (!strncmp(line, "# rcc-function", 14)) {
            struct ld_function *row, *rows = ld_functions.data;
            p = line + 14;
            if (!isspace((unsigned char)*p))
                ld_error("invalid function metadata directive");
            token = decimal(&p, 8191);
            arity = decimal(&p, 64);
            space(&p);
            for (i = 0; *p && !isspace((unsigned char)*p); i++) {
                if (i >= sizeof name - 1)
                    ld_error("metadata symbol exceeds capacity");
                name[i] = *p++;
            }
            name[i] = 0;
            space(&p);
            if (!token || *p || !(isalpha((unsigned char)name[0]) || name[0] == '_'))
                ld_error("invalid function metadata");
            for (i = 0; name[i]; i++)
                if (!isalnum((unsigned char)name[i]) && name[i] != '_')
                    ld_error("invalid function symbol in metadata");
            for (i = 0; i < ld_functions.count; i++)
                if (rows[i].token == token || !strcmp(rows[i].name, name))
                    ld_error("duplicate continuation token/symbol");
            row = ld_append(&ld_functions, sizeof *row);
            row->token = token;
            row->arity = arity;
            row->name = ld_memory(strlen(name) + 1);
            strcpy(row->name, name);
        } else if (!strncmp(line, "# rcc-", 6))
            ld_error("unsupported continuation metadata directive");
    }
    if (ferror(f) || fclose(f))
        ld_error("cannot finish metadata input");
    if (!ld_entry || !ld_functions.count)
        ld_error("missing continuation metadata");
    for (i = 0; i < ld_functions.count; i++) {
        struct ld_function *r = &((struct ld_function *)ld_functions.data)[i];
        if (r->token == ld_entry) {
            if (r->arity != 1)
                ld_error("entry must have arity one");
            return;
        }
    }
    ld_error("entry has no function metadata");
}
