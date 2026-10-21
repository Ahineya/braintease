#include "native.h"
#include <string.h>
#include <stdlib.h>

static void octet(struct vec *v, unsigned value)
{
    if (v->count >= 16777216UL)
        fail("native module metadata exceeds 16 MiB");
    *(unsigned char *)append(v, sizeof(unsigned char)) = value & 255;
}

static void number(struct vec *v, unsigned long long value, unsigned width)
{
    while (width--) {
        octet(v, (unsigned)value);
        value >>= 8;
    }
}

static void text(struct vec *v, const char *s)
{
    do {
        if ((unsigned char)*s > 127)
            fail("native metadata names must use ASCII");
        octet(v, (unsigned char)*s);
    } while (*s++);
}

static void type_text(struct vec *v, struct type *t, unsigned depth)
{
    char n[32];
    size_t i;
    if (!t || depth > 32)
        fail("invalid/excessively nested native ABI type");
    if (t->kind == TYARRAY) {
        snprintf(n, sizeof n, "a%lu[", t->size);
        for (i = 0; n[i]; i++)
            octet(v, n[i]);
        type_text(v, t->base, depth + 1);
        octet(v, ']');
    } else if (t->kind == TYSTRUCT) {
        struct type **fields = t->fields.data;
        octet(v, '{');
        for (i = 0; i < t->fields.count; i++) {
            type_text(v, fields[i], depth + 1);
            octet(v, ';');
        }
        octet(v, '}');
    } else {
        char kind = t->kind == TYVOID      ? 'v'
                    : t->kind == TYINT     ? 'i'
                    : t->kind == TYFLOAT   ? 'f'
                    : t->kind == TYPOINTER ? 'p'
                                           : '?';
        if (kind == '?' || (kind != 'v' && !t->bits))
            fail("unsupported native ABI type");
        snprintf(n, sizeof n, "%c%u", kind, t->bits);
        for (i = 0; n[i]; i++)
            octet(v, n[i]);
    }
    if (v->count > 1023)
        fail("native ABI signature exceeds 1023 octets");
}

char *native_signature(struct function *f)
{
    struct vec v = {0};
    struct parameter *params = f->parameters.data;
    char *result;
    size_t i;
    if (f->parameters.count + (f->vararg != 0) > 64)
        fail("native module function exceeds 64 lowered arguments");
    type_text(&v, f->result, 0);
    octet(&v, '(');
    for (i = 0; i < f->parameters.count; i++) {
        if (i)
            octet(&v, ',');
        type_text(&v, params[i].type, 0);
    }
    if (f->vararg) {
        if (f->parameters.count)
            octet(&v, ',');
        text(&v, "...");
        v.count--; /* text writes NUL; the signature continues below. */
    }
    octet(&v, ')');
    octet(&v, 0);
    result = duplicate(v.data);
    free(v.data);
    return result;
}

struct function *native_declaration(struct module *m, const char *name)
{
    struct function *f = m->functions.data, *found = NULL;
    size_t i;
    for (i = 0; i < m->functions.count; i++)
        if (!strcmp(f[i].name, name)) {
            if (found && strcmp(native_signature(found), native_signature(&f[i])))
                fail("incompatible native declarations for '%s'", name);
            if (!found || !f[i].external)
                found = &f[i];
        }
    return found;
}

static struct native_binding *binding(struct native_output *o, unsigned kind, const char *name)
{
    struct native_binding *b = o->bindings.data;
    size_t i;
    for (i = 0; i < o->bindings.count; i++)
        if (!strcmp(b[i].name, name)) {
            if (b[i].kind != kind)
                fail("native symbol used as both function and object: '%s'", name);
            return &b[i];
        }
    b = append(&o->bindings, sizeof *b);
    b->name = duplicate(name);
    b->kind = kind;
    if (!strcmp(name, "__rcc_start")) {
        b->flags = NM_DEFINED | NM_ENTRY;
        b->cells = 1;
        b->signature = "i64(p32)";
    } else {
        struct symbol *s = symbol(o->module, name);
        b->flags = (s->defined ? NM_DEFINED : 0) | (s->internal ? NM_INTERNAL : 0);
        if (kind == NM_FUNCTION) {
            struct function *f = native_declaration(o->module, name);
            if (!f)
                fail("function '%s' has no native ABI declaration", name);
            b->cells = f->parameters.count + (f->vararg != 0);
            b->signature = native_signature(f);
        } else {
            struct vec v = {0};
            if (!s->type || s->type->size > 1023999UL ||
                (!s->type->size && (s->defined || s->type->kind != TYARRAY)))
                fail("global '%s' has no complete/bounded native type", name);
            b->cells = s->type->size;
            /* Frontend aggregate declarations are flattened to cell arrays,
             * while definitions describe initializer storage chunks. Their
             * stable IR ABI is the total storage extent, not source C fields. */
            if (s->type->kind == TYARRAY || s->type->kind == TYSTRUCT) {
                char storage[32];
                snprintf(storage, sizeof storage, "m%lu", s->type->size);
                text(&v, storage);
            } else {
                type_text(&v, s->type, 0);
                octet(&v, 0);
            }
            b->signature = duplicate(v.data);
            free(v.data);
        }
    }
    return b;
}

static void used(struct native_output *o, struct value *v)
{
    size_t i;
    if (!v)
        return;
    if (v->kind == VFUNCTION || v->kind == VGLOBAL)
        binding(o, v->kind == VFUNCTION ? NM_FUNCTION : NM_GLOBAL, v->name);
    if (v->base)
        used(o, v->base);
    for (i = 0; i < v->items.count; i++)
        used(o, ((struct value **)v->items.data)[i]);
}

static void record(struct native_output *o, unsigned kind, unsigned flags, unsigned long cells,
                   const char *name, const char *sig, unsigned long offset,
                   unsigned long long value, unsigned width, unsigned mode)
{
    size_t n = strlen(name) + 1, s = strlen(sig) + 1;
    size_t payload = kind == NM_CELLS ? o->constants.count : 0;
    unsigned long size = (NM_RECORD + n + s + payload + 7) & ~7UL;
    number(&o->metadata, kind, 4);
    number(&o->metadata, flags, 4);
    number(&o->metadata, size, 4);
    number(&o->metadata, cells, 4);
    number(&o->metadata, 0, 8);
    number(&o->metadata, offset, 8);
    number(&o->metadata, value, 8);
    number(&o->metadata, width, 4);
    number(&o->metadata, mode, 4);
    number(&o->metadata, n, 4);
    number(&o->metadata, s, 4);
    number(&o->metadata, 0, 8);
    text(&o->metadata, name);
    text(&o->metadata, sig);
    if (payload) {
        size_t i;
        for (i = 0; i < payload; i++)
            octet(&o->metadata, ((unsigned char *)o->constants.data)[i]);
    }
    while (o->metadata.count % 8)
        octet(&o->metadata, 0);
    o->records++;
}

static void flush_constants(struct native_output *o)
{
    if (!o->constants.count)
        return;
    record(o, NM_CELLS, 0, o->constants.count / 2, o->constant_name, "", o->constant_offset, 0, 0,
           0);
    o->constants.count = 0;
}

static void constant(struct native_output *o, const char *name, unsigned long at,
                     unsigned long long value, unsigned width)
{
    unsigned i;
    /* Zero cells already belong to the loader's zeroed global template.
     * Keep nonzero runs compact without storing enormous explicit zero tails. */
    if (!value) {
        flush_constants(o);
        return;
    }
    if (o->constants.count &&
        (o->constant_name != name || at != o->constant_offset + o->constants.count / 2))
        flush_constants(o);
    if (!o->constants.count) {
        o->constant_name = name;
        o->constant_offset = at;
    }
    for (i = 0; i < width; i++, value >>= 16)
        number(&o->constants, value & 65535, 2);
}

static void initializer(struct native_output *o, const char *name, struct type *t, struct value *v,
                        unsigned long at)
{
    size_t i;
    const char *target = "";
    unsigned mode = 0;
    unsigned long long value = 0;
    if (!v || v->kind == VUNDEF)
        return;
    if (t->kind == TYARRAY || t->kind == TYSTRUCT) {
        struct value **items = v->items.data;
        unsigned long offset = 0;
        if ((t->kind == TYARRAY && v->kind != VARRAY) ||
            (t->kind == TYSTRUCT && v->kind != VAGGREGATE))
            fail("invalid aggregate native initializer");
        for (i = 0; i < v->items.count; i++) {
            struct type *field;
            if (t->kind == TYARRAY) {
                field = t->base;
                if (!field->size || i >= t->size / field->size)
                    fail("native array initializer exceeds its type");
            } else {
                if (i >= t->fields.count)
                    fail("native struct initializer exceeds its type");
                field = ((struct type **)t->fields.data)[i];
            }
            initializer(o, name, field, items[i], at + offset);
            offset += field->size;
        }
        return;
    }
    if (!t->bits || !t->size || t->size > 4)
        fail("unsupported native scalar initializer");
    while (v->kind == VOFFSET || v->kind == VFAT) {
        if (v->kind == VOFFSET) {
            value += (unsigned long long)(long long)v->offset;
            v = v->base;
        } else if (v->bank == BMIXED || (v->bank == BGLOBAL && native_global_reference(v->base)))
            v = v->base;
        else if (v->bank == BNULL) {
            v = NULL;
            break;
        } else if (v->base->kind == VNUMBER && v->base->bits < 64000 && v->bank >= 0) {
            value += ((unsigned long long)(unsigned)v->bank << 16) | v->base->bits;
            v = NULL;
            break;
        } else
            fail("unresolved pointer tag in native initializer");
    }
    if (v && (v->kind == VGLOBAL || v->kind == VFUNCTION)) {
        if (t->kind != TYPOINTER || t->size != 2)
            fail("symbolic native initializer requires a pointer");
        target = v->name;
        mode = v->kind == VGLOBAL ? 1 : 2;
        if (mode == 2 && value)
            fail("function token initializer cannot have an offset");
    } else if (v && v->kind == VNUMBER)
        value += v->bits;
    else if (v)
        fail("unsupported native initializer value");
    if (!mode && t->bits < 64)
        value &= (1ULL << t->bits) - 1;
    if (!mode)
        constant(o, name, at, value, (unsigned)t->size);
    else {
        flush_constants(o);
        record(o, NM_INITIALIZER, 0, 0, name, target, at, value, (unsigned)t->size, mode);
    }
}

void native_metadata(struct native_output *o)
{
    struct function *functions = o->module->functions.data;
    struct symbol *symbols = o->module->symbols.data;
    size_t i, j, k;
    for (i = 0; i < NM_HEADER; i++)
        octet(&o->metadata, 0);
    if (!o->no_start && native_find_function(o->module, "main"))
        binding(o, NM_FUNCTION, "__rcc_start");
    for (i = 0; i < o->module->symbols.count; i++) {
        if (!strcmp(symbols[i].name, "__rcc_start"))
            fail("reserved native symbol '__rcc_start'");
        if (symbols[i].defined)
            binding(o, symbols[i].function ? NM_FUNCTION : NM_GLOBAL, symbols[i].name);
        used(o, symbols[i].init);
    }
    for (i = 0; i < o->module->functions.count; i++) {
        struct block *blocks = functions[i].blocks.data;
        for (j = 0; j < functions[i].blocks.count; j++) {
            struct instruction **ins = blocks[j].instructions.data;
            for (k = 0; k < blocks[j].instructions.count; k++) {
                size_t n;
                if (strcmp(ins[k]->op, "intrinsic") &&
                    !(ins[k]->a && ins[k]->a->kind == VFUNCTION && !strcmp(ins[k]->op, "call") &&
                      (native_service(o->module, ins[k]->a->name) ||
                       (o->kernel && !native_find_function(o->module, ins[k]->a->name)))))
                    used(o, ins[k]->a);
                used(o, ins[k]->b);
                used(o, ins[k]->c);
                for (n = 0; n < ins[k]->args.count; n++)
                    used(o, ((struct value **)ins[k]->args.data)[n]);
            }
        }
    }
    for (i = 0; i < o->bindings.count; i++) {
        struct native_binding *b = &((struct native_binding *)o->bindings.data)[i];
        b->offset = o->metadata.count;
        if (b->flags & NM_ENTRY)
            o->entry_record = o->records + 1;
        record(o, b->kind, b->flags, b->cells, b->name, b->signature, 0, 0, 0, 0);
    }
    for (i = 0; i < o->module->symbols.count; i++)
        if (symbols[i].defined && !symbols[i].function) {
            initializer(o, symbols[i].name, symbols[i].type, symbols[i].init, 0);
            flush_constants(o);
        }
    free(o->constants.data);
    o->constants.data = NULL;
}

char *native_binding_value(struct native_output *o, unsigned kind, const char *name)
{
    struct native_binding *b = o->bindings.data;
    char *pointer, *result;
    size_t i;
    for (i = 0; i < o->bindings.count; i++)
        if (b[i].kind == kind && !strcmp(b[i].name, name))
            break;
    if (i == o->bindings.count)
        fail("missing native binding '%s'", name);
    pointer = native_temporary(o);
    result = native_temporary(o);
    if (o->kernel) {
        char number[32];
        unsigned char *p = (unsigned char *)o->metadata.data + b[i].offset + 16;
        unsigned long long bits = 0;
        unsigned n;
        for (n = 0; n < 8; n++)
            bits |= (unsigned long long)p[n] << (n * 8);
        if (!bits)
            fail("unresolved kernel binding '%s'", name);
        snprintf(number, sizeof number, "%llu", bits);
        return native_copy(number);
    }
    fprintf(o->file, "    %s =l add $%s, %lu\n    %s =l loadl %s\n", pointer, NM_SYMBOL,
            b[i].offset + 16, result, pointer);
    return result;
}

void native_metadata_write(struct native_output *o)
{
    unsigned char *data = o->metadata.data;
    size_t i;
    unsigned long values[3];
    char chunk[4 * 256 + 8];
    memcpy(data, NM_MAGIC, 8);
    values[0] = o->metadata.count;
    values[1] = o->records;
    values[2] = o->entry_record;
    for (i = 0; i < 3; i++) {
        unsigned n;
        for (n = 0; n < 4; n++)
            data[8 + i * 4 + n] = (values[i] >> (n * 8)) & 255;
    }
    fprintf(o->file, "data $%s = align 8 {\n", NM_SYMBOL);
    /* Bounded escaped byte strings avoid a variadic formatter call for every
     * metadata octet. fwrite also avoids one resumable stdio call per output
     * character. Octal escapes are unambiguous even for NUL and high bytes. */
    for (i = 0; i < o->metadata.count;) {
        size_t count = 0, used = 0;
        chunk[used++] = ' ';
        chunk[used++] = 'b';
        chunk[used++] = ' ';
        chunk[used++] = '"';
        while (i < o->metadata.count && count++ < 256) {
            unsigned byte = data[i++];
            chunk[used++] = '\\';
            chunk[used++] = (char)('0' + (byte >> 6));
            chunk[used++] = (char)('0' + ((byte >> 3) & 7));
            chunk[used++] = (char)('0' + (byte & 7));
        }
        chunk[used++] = '"';
        chunk[used++] = ',';
        chunk[used++] = '\n';
        if (fwrite(chunk, 1, used, o->file) != used)
            fail("cannot write native module metadata");
    }
    fprintf(o->file, "\n}\n");
}
