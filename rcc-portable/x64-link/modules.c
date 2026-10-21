#include "link.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct module_record {
    struct object_module *owner;
    struct module_record *definition, *next, *local_next;
    const char *name, *signature;
    const unsigned char *payload;
    L32 kind, flags, cells, slot, offset, width, mode, assigned;
    L64 value;
    struct ld_symbol *symbol;
};

struct object_module {
    struct ld_object *object;
    struct ld_symbol *symbol;
    struct ld_vector records;
    L32 length, entry;
    struct module_record *bindings[256];
};

static struct ld_vector modules;
static struct module_record *publics[4096];
struct ld_vector ld_initializers;
L32 ld_global_bank = 1, ld_global_cells = 1;
const char *ld_entry_symbol;

static L32 u32(const unsigned char *p)
{
    L64 n = ld_read(p, 4);
    if (n > LD_LIMIT)
        ld_error("module metadata quantity exceeds capacity");
    return (L32)n;
}

static unsigned bucket(const char *s)
{
    unsigned h = 5381;
    while (*s)
        h = (h * 33 + (unsigned char)*s++) & 4095;
    return h;
}

struct ld_symbol *ld_local_symbol(struct ld_object *o, const char *name)
{
    struct ld_symbol *result = NULL;
    L32 i;
    for (i = 1; i < o->symbol_count; i++)
        if (!strcmp(o->symbols[i].name, name)) {
            if (result)
                ld_error("duplicate native symbol name '%s'", name);
            result = &o->symbols[i];
        }
    return result;
}

static const char *string(const unsigned char *p, L32 size)
{
    L32 i;
    if (!size || size > 1024 || p[size - 1])
        ld_error("invalid module metadata string");
    for (i = 0; i + 1 < size; i++)
        if (!p[i] || p[i] > 127)
            ld_error("invalid/non-ASCII module metadata string");
    return (const char *)p;
}

static void identifier(const char *s)
{
    if (!isalpha((unsigned char)*s) && *s != '_')
        ld_error("invalid module symbol name");
    while (*s) {
        if (!isalnum((unsigned char)*s) && *s != '_')
            ld_error("invalid module symbol name");
        s++;
    }
}

static L32 digits(const char **text)
{
    L32 n = 0;
    if (!isdigit((unsigned char)**text))
        ld_error("invalid native ABI signature number");
    while (isdigit((unsigned char)**text)) {
        unsigned d = (unsigned)(*(*text)++ - '0');
        if (n > 1024000UL / 10 || (n == 1024000UL / 10 && d > 1024000UL % 10))
            ld_error("native ABI signature number exceeds capacity");
        n = n * 10 + d;
    }
    return n;
}

static L32 type(const char **text, unsigned depth, int scalar, int allow_void)
{
    char kind = *(*text)++;
    L32 n, cells = 0;
    if (!kind || depth > 32)
        ld_error("invalid/excessively nested native ABI signature");
    if (kind == '{' && scalar != 1) {
        while (**text && **text != '}') {
            n = type(text, depth + 1, 0, 0);
            if (n > 1024000UL - cells)
                ld_error("native struct ABI size overflow");
            cells += n;
            if (*(*text)++ != ';')
                ld_error("invalid native struct ABI signature");
        }
        if (*(*text)++ != '}')
            ld_error("unterminated native struct ABI signature");
        return cells;
    }
    n = digits(text);
    if (kind == 'm' && !scalar)
        return n;
    if (kind == 'a' && scalar != 1) {
        L32 element;
        if (*(*text)++ != '[')
            ld_error("invalid native array ABI signature");
        element = type(text, depth + 1, 0, 0);
        if (*(*text)++ != ']' || !element || n % element)
            ld_error("invalid native array ABI size");
        return n;
    }
    if (kind == 'v' && !n && allow_void)
        return 0;
    if ((kind == 'i' && (n == 1 || n == 8 || n == 16 || n == 32 || n == 64)) ||
        (kind == 'f' && (n == 32 || n == 64)) || (kind == 'p' && n == 32))
        return (n + 15) / 16;
    ld_error("unsupported native ABI signature type");
    return 0;
}

static void signature(struct module_record *r)
{
    const char *p = r->signature;
    /* Function aggregate results are logical-pointer carriers whose layout
     * remains in the signature. Parameters are still explicit scalar carriers;
     * m<size> is reserved for erased global storage, never a result type. */
    L32 size = type(&p, 0, r->kind == NM_FUNCTION ? 2 : 0, r->kind == NM_FUNCTION), count = 0;
    if (r->kind == NM_FUNCTION) {
        if (!size && r->signature[0] != 'v')
            ld_error("empty aggregate native result");
        if (*p++ != '(')
            ld_error("invalid native function signature");
        while (*p && *p != ')') {
            if (!strncmp(p, "...", 3)) {
                p += 3;
                count++; /* One hidden logical cursor, not native SysV varargs. */
                if (*p != ')')
                    ld_error("variadic marker must end a function signature");
                break;
            }
            type(&p, 0, 1, 0);
            count++;
            if (*p == ',')
                p++;
            else if (*p != ')')
                ld_error("invalid native parameter signature");
            else
                break;
            if (*p == ')')
                ld_error("trailing native parameter comma");
        }
        if (*p++ != ')' || *p || count != r->cells || count > 64)
            ld_error("native function arity/signature mismatch");
    } else if (*p || size != r->cells || (r->flags & NM_DEFINED && !size))
        ld_error("native global size/signature mismatch");
}

static struct object_module *read_module(struct ld_object *o, struct ld_symbol *s)
{
    struct object_module *m = ld_memory(sizeof *m);
    struct ld_section *section;
    const unsigned char *data;
    L32 count, at = NM_HEADER, i;
    m->object = o;
    m->symbol = s;
    if (!s->section || s->section >= o->section_count || s->binding || s->value % 8)
        ld_error("module metadata must be local aligned ELF data");
    section = &o->sections[s->section];
    if (section->type != 1 || !(section->flags & 2) || section->size - s->value < NM_HEADER)
        ld_error("invalid module metadata section/span");
    data = ld_section_data(o, section) + (L32)s->value;
    m->length = u32(data + 8);
    count = u32(data + 12);
    m->entry = u32(data + 16);
    if (memcmp(data, NM_MAGIC, 8) || m->length < NM_HEADER || m->length % 8 ||
        m->length > section->size - s->value || (s->size && s->size != m->length) ||
        ld_read(data + 20, 4) || ld_read(data + 24, 8) || m->entry > count ||
        count > (m->length - NM_HEADER) / NM_RECORD)
        ld_error("invalid/unsupported RCCMOD01 header");
    for (i = 0; i < count; i++) {
        struct module_record *r = ld_memory(sizeof *r);
        struct module_record **slot = ld_append(&m->records, sizeof *slot);
        const unsigned char *p;
        L32 size, names, sigs, j;
        *slot = r;
        if (at > m->length || NM_RECORD > m->length - at)
            ld_error("truncated module record");
        p = data + at;
        r->owner = m;
        r->kind = u32(p);
        r->flags = u32(p + 4);
        size = u32(p + 8);
        r->cells = u32(p + 12);
        r->slot = at + 16;
        r->offset = u32(p + 24);
        r->value = ld_read(p + 32, 8);
        r->width = u32(p + 40);
        r->mode = u32(p + 44);
        names = u32(p + 48);
        sigs = u32(p + 52);
        if (size < NM_RECORD || size % 8 || size > m->length - at || names > size - NM_RECORD ||
            sigs > size - NM_RECORD - names || ld_read(p + 16, 8) || ld_read(p + 28, 4) ||
            ld_read(p + 56, 8))
            ld_error("invalid module record span/reserved fields");
        r->name = string(p + NM_RECORD, names);
        r->signature = string(p + NM_RECORD + names, sigs);
        identifier(r->name);
        j = NM_RECORD + names + sigs;
        if (r->kind == NM_CELLS) {
            if (r->flags || r->value || r->width || r->mode || *r->signature || !r->cells ||
                r->cells > 1023999UL || r->cells > (size - j) / 2)
                ld_error("invalid/truncated constant cell run");
            r->payload = p + j;
            j += r->cells * 2;
        }
        for (; j < size; j++)
            if (p[j])
                ld_error("nonzero module record padding");
        if (r->kind == NM_FUNCTION || r->kind == NM_GLOBAL) {
            if (r->flags & ~(NM_DEFINED | NM_INTERNAL | NM_ENTRY) || r->offset || r->value ||
                r->width || r->mode || (r->kind == NM_GLOBAL && (r->flags & NM_ENTRY)) ||
                (r->flags & NM_ENTRY && (r->flags != (NM_DEFINED | NM_ENTRY) ||
                                         strcmp(r->name, "__rcc_start") || r->cells != 1)))
                ld_error("invalid native binding flags/fields");
            if ((r->flags & NM_ENTRY) && strcmp(r->signature, "i64(p32)"))
                ld_error("invalid native entry signature");
            if (r->kind == NM_GLOBAL && r->cells > 1023999UL)
                ld_error("logical globals exceed sixteen banks");
            signature(r);
            {
                unsigned h = bucket(r->name) & 255;
                struct module_record *previous;
                for (previous = m->bindings[h]; previous; previous = previous->local_next)
                    if (!strcmp(previous->name, r->name))
                        ld_error("duplicate module binding '%s'", r->name);
                r->local_next = m->bindings[h];
                m->bindings[h] = r;
            }
        } else if (r->kind == NM_INITIALIZER) {
            if (r->flags || r->cells || !r->width || r->width > 4 || r->mode > 2 ||
                (r->mode && r->width != 2) || (r->mode == 2 && r->value) ||
                (!r->mode && *r->signature))
                ld_error("invalid native initializer record");
            if (r->mode)
                identifier(r->signature);
        } else if (r->kind != NM_CELLS)
            ld_error("unknown module record kind");
        if ((i + 1 == m->entry) != ((r->flags & NM_ENTRY) != 0))
            ld_error("module entry record mismatch");
        at += size;
    }
    if (at != m->length)
        ld_error("trailing module metadata");
    return m;
}

void ld_object_symbols(struct ld_object *o, void (*visit)(const char *, int, void *), void *context)
{
    struct ld_symbol *s = ld_local_symbol(o, NM_SYMBOL);
    size_t i;
    if (s) {
        struct object_module *m = read_module(o, s);
        struct module_record **rs = m->records.data;
        for (i = 0; i < m->records.count; i++) {
            struct module_record *r = rs[i];
            if (r->kind < NM_INITIALIZER && !(r->flags & NM_INTERNAL))
                visit(r->name, (r->flags & NM_DEFINED) != 0, context);
            free(r);
        }
        free(m->records.data);
        free(m);
    }
    for (i = 1; i < o->symbol_count; i++)
        if (o->symbols[i].binding)
            visit(o->symbols[i].name, o->symbols[i].section != 0, context);
}

unsigned ld_program_arity(void)
{
    struct ld_object **objects = ld_objects.data;
    unsigned arity = 0, found = 0;
    size_t i, j;
    for (i = 0; i < ld_objects.count; i++) {
        struct ld_symbol *s = ld_local_symbol(objects[i], NM_SYMBOL);
        struct object_module *m;
        struct module_record **rs;
        if (!s)
            continue;
        ld_path = objects[i]->path;
        m = read_module(objects[i], s);
        rs = m->records.data;
        if (m->entry)
            ld_error("SDK objects must be compiled with --no-start (use cc --sdk)");
        for (j = 0; j < m->records.count; j++) {
            struct module_record *r = rs[j];
            if (r->kind == NM_FUNCTION && (r->flags & NM_DEFINED) && !strcmp(r->name, "main")) {
                if (found++ || (r->flags & NM_INTERNAL))
                    ld_error("SDK link requires one public main definition");
                if (!strcmp(r->signature, "i16()"))
                    arity = 0;
                else if (!strcmp(r->signature, "i16(i16,p32)"))
                    arity = 2;
                else if (!strcmp(r->signature, "i16(i16,p32,p32)"))
                    arity = 3;
                else
                    ld_error("unsupported SDK main signature");
            }
            free(r);
        }
        free(m->records.data);
        free(m);
    }
    if (!found)
        ld_error("SDK link requires a main definition");
    return arity;
}

static struct module_record *local(struct object_module *m, const char *name)
{
    struct module_record *r;
    for (r = m->bindings[bucket(name) & 255]; r; r = r->local_next)
        if (!strcmp(r->name, name))
            return r;
    return NULL;
}

static struct module_record *resolve(struct module_record *r)
{
    struct module_record *d;
    if (r->flags & NM_DEFINED)
        return r;
    if (r->flags & NM_INTERNAL)
        ld_error("undefined internal symbol '%s'", r->name);
    for (d = publics[bucket(r->name)]; d; d = d->next)
        if (!strcmp(d->name, r->name))
            return d;
    ld_error("undefined C symbol '%s'", r->name);
    return NULL;
}

static int compatible(struct module_record *ref, struct module_record *def)
{
    if (ref->kind != def->kind)
        return 0;
    if (!strcmp(ref->signature, def->signature))
        return 1;
    /* Aggregate source types are erased by RCC IR's initializer-storage
     * representation. Check the cell extent; incomplete extern arrays get
     * their extent from the defining unit. Scalar ABI stays typed. */
    if (ref->kind == NM_GLOBAL && !ref->cells && !strcmp(ref->signature, "m0") &&
        def->signature[0] == 'm')
        return 1;
    return 0;
}

void ld_modules(int legacy)
{
    struct ld_object **objects = ld_objects.data;
    struct object_module **ms;
    size_t i, j;
    for (i = 0; i < ld_objects.count; i++) {
        struct ld_symbol *s = ld_local_symbol(objects[i], NM_SYMBOL);
        ld_path = objects[i]->path;
        if (s) {
            if (legacy)
                ld_error("cannot mix sidecar metadata with relocatable modules");
            struct object_module **slot = ld_append(&modules, sizeof *slot);
            *slot = read_module(objects[i], s);
        }
    }
    if (legacy)
        return;
    if (!modules.count)
        ld_error("no relocatable module metadata (legacy objects need --metadata)");
    ms = modules.data;
    /* Assign final module-local IDs and logical storage only to definitions. */
    for (i = 0; i < modules.count; i++) {
        struct module_record **rs = ms[i]->records.data;
        ld_path = ms[i]->object->path;
        for (j = 0; j < ms[i]->records.count; j++) {
            struct module_record *r = rs[j], *previous;
            if (r->kind >= NM_INITIALIZER || !(r->flags & NM_DEFINED))
                continue;
            if (!(r->flags & NM_INTERNAL)) {
                unsigned h = bucket(r->name);
                for (previous = publics[h]; previous; previous = previous->next)
                    if (!strcmp(previous->name, r->name))
                        ld_error("duplicate C definition '%s'", r->name);
                r->next = publics[h];
                publics[h] = r;
            }
            if (r->kind == NM_FUNCTION) {
                struct ld_function *fn;
                char *name = ld_memory(strlen(r->name) + 16);
                sprintf(name, "%s%s", r->flags & NM_ENTRY ? "" : "rcc_user_",
                        r->flags & NM_ENTRY ? "rcc_start_entry" : r->name);
                r->symbol = ld_local_symbol(ms[i]->object, name);
                if (!r->symbol || !r->symbol->section ||
                    r->symbol->section >= ms[i]->object->section_count ||
                    !(ms[i]->object->sections[r->symbol->section].flags & 4) ||
                    r->symbol->value >= ms[i]->object->sections[r->symbol->section].size ||
                    r->symbol->binding != ((r->flags & NM_INTERNAL) ? 0U : 1U))
                    ld_error("missing/incompatible native definition '%s'", r->name);
                if (ld_functions.count >= 8191)
                    ld_error("too many native functions");
                fn = ld_append(&ld_functions, sizeof *fn);
                fn->token = (L32)ld_functions.count;
                fn->arity = r->cells;
                fn->name = name;
                fn->symbol = r->symbol;
                r->assigned = 0x80000000UL | fn->token;
                if ((r->flags & NM_ENTRY) ||
                    (ld_entry_symbol && !strcmp(r->name, ld_entry_symbol))) {
                    if (ld_entry_symbol && (r->flags & NM_ENTRY))
                        ld_error("cannot combine minimal and libc startup");
                    if (ld_entry_symbol && strcmp(r->signature, "v0(i32)"))
                        ld_error("invalid libc entry signature");
                    if (ld_entry)
                        ld_error("multiple native entry points");
                    ld_entry = fn->token;
                }
            } else {
                if (r->cells > 1024000UL - ld_global_cells)
                    ld_error("logical globals exceed sixteen banks");
                r->assigned = ld_global_cells;
                ld_global_cells += r->cells;
            }
        }
    }
    if (!ld_entry)
        ld_error("no main/native entry point");
    if (ld_global_cells > 64000)
        ld_global_bank = 32;
    for (i = 0; i < modules.count; i++) {
        struct module_record **rs = ms[i]->records.data;
        ld_path = ms[i]->object->path;
        for (j = 0; j < ms[i]->records.count; j++) {
            struct module_record *r = rs[j];
            if (r->kind >= NM_INITIALIZER)
                continue;
            r->definition = resolve(r);
            if (!compatible(r, r->definition))
                ld_error("incompatible C ABI for '%s'", r->name);
        }
    }
}

static L32 logical(L64 index)
{
    if (index > 1024000UL)
        ld_error("logical initializer address overflow");
    return ((ld_global_bank + (L32)index / 64000) << 16) | ((L32)index % 64000);
}

static int compare_initializers(const void *a, const void *b)
{
    L32 x = ((const struct ld_initializer *)a)->pointer,
        y = ((const struct ld_initializer *)b)->pointer;
    return x < y ? -1 : x > y;
}

void ld_modules_patch(void)
{
    struct object_module **ms = modules.data;
    size_t i, j;
    for (i = 0; i < modules.count; i++) {
        struct object_module *m = ms[i];
        struct module_record **rs = m->records.data;
        L32 base = (L32)ld_address(m->symbol);
        ld_path = m->object->path;
        for (j = 0; j < m->records.count; j++) {
            struct module_record *r = rs[j];
            if (r->kind < NM_INITIALIZER) {
                struct module_record *d = r->definition;
                ld_image_patch(base + r->slot,
                               d->kind == NM_FUNCTION ? d->assigned : logical(d->assigned), 8);
            } else {
                struct module_record *dest = local(m, r->name), *target;
                struct ld_initializer *v;
                L64 bits = r->value;
                L32 count = r->kind == NM_CELLS ? r->cells : r->width;
                if (!dest || dest->kind != NM_GLOBAL || !(dest->flags & NM_DEFINED) ||
                    r->offset > dest->cells || count > dest->cells - r->offset)
                    ld_error("native initializer outside its defining global");
                if (r->kind == NM_CELLS) {
                    L32 at;
                    for (at = 0; at < r->cells;) {
                        unsigned width = r->cells - at > 4 ? 4 : (unsigned)(r->cells - at);
                        v = ld_append(&ld_initializers, sizeof *v);
                        v->pointer = logical((L64)dest->assigned + r->offset + at);
                        v->width = width;
                        v->value = ld_read(r->payload + at * 2, width * 2);
                        at += width;
                    }
                    continue;
                }
                if (r->mode) {
                    target = local(m, r->signature);
                    if (!target ||
                        (r->mode == 1 ? target->kind != NM_GLOBAL : target->kind != NM_FUNCTION))
                        ld_error("initializer references an undeclared/incompatible symbol");
                    target = target->definition;
                    bits = r->mode == 1 ? logical((L64)target->assigned + bits) : target->assigned;
                } else if (r->width < 4 && bits >> (r->width * 16))
                    ld_error("native initializer exceeds its declared width");
                v = ld_append(&ld_initializers, sizeof *v);
                v->pointer = logical((L64)dest->assigned + r->offset);
                v->width = r->width;
                v->value = bits;
                v->function = r->mode == 2;
            }
        }
    }
    if (ld_initializers.count > 1)
        qsort(ld_initializers.data, ld_initializers.count, sizeof(struct ld_initializer),
              compare_initializers);
    for (i = 1; i < ld_initializers.count; i++) {
        struct ld_initializer *a = ld_initializers.data;
        L32 before = (a[i - 1].pointer >> 16) * 64000 + (a[i - 1].pointer & 65535);
        L32 after = (a[i].pointer >> 16) * 64000 + (a[i].pointer & 65535);
        if (after < before + a[i - 1].width)
            ld_error("overlapping logical initializers");
    }
}

void ld_modules_relocation(struct ld_object *o, unsigned section, L64 offset, unsigned width)
{
    struct object_module **ms = modules.data;
    size_t i;
    for (i = 0; i < modules.count; i++) {
        struct object_module *m = ms[i];
        if (m->object == o && m->symbol->section == section &&
            offset < m->symbol->value + m->length && offset + width > m->symbol->value)
            ld_error("native ELF relocation overlaps compiler module metadata");
    }
}
