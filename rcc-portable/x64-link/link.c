#include "link.h"
#include <string.h>
#include <stdlib.h>

/* Import only the checked continuation ABI used by the current lowerer.
 * Adding SDK imports requires an explicit runtime/arity agreement. */
static const struct {
    const char *name;
    unsigned arity;
} helpers[] = {{"rcc_native_poll", 0},
               {"rcc_native_load", 2},
               {"rcc_native_store", 3},
               {"rcc_native_gep", 2},
               {"rcc_native_image_token", 1},
               {"rcc_native_process_frame", 1},
               {"rcc_native_process_pc", 0},
               {"rcc_native_process_arg", 1},
               {"rcc_native_process_slot", 1},
               {"rcc_native_process_set_pc", 1},
               {"rcc_native_process_save", 2},
               {"rcc_native_process_argument", 2},
               {"rcc_native_process_call", 3},
               {"rcc_native_process_return", 1},
               {"rcc_native_process_checkpoint", 0},
               {"rcc_native_process_request", 4},
               {"rcc_native_process_bad_pc", 0}};

#define NH (sizeof helpers / sizeof helpers[0])
static struct ld_symbol *globals[4096];
static long imports[NH];

struct patch_span {
    L32 at, width;
};
static struct ld_vector patches;
static L32 trampolines;
L32 ld_code_size, ld_bindings, ld_helper_count;

static unsigned hash(const char *name)
{
    L32 h = 5381;
    while (*name)
        h = ((h << 5) + h + (unsigned char)*name++) & 4095;
    return (unsigned)h;
}

static struct ld_symbol *find(const char *name)
{
    struct ld_symbol *s = globals[hash(name)];
    while (s && strcmp(s->name, name))
        s = s->next;
    return s;
}

static long helper(const char *name)
{
    size_t i;
    for (i = 0; i < NH; i++)
        if (!strcmp(name, helpers[i].name))
            return (long)i;
    return -1;
}

static L32 reserve(L32 alignment, L32 size)
{
    L32 at;
    if (!alignment)
        alignment = 1;
    at = (ld_code_size + alignment - 1) & ~(alignment - 1);
    if (at > LD_LIMIT || size > LD_LIMIT - at)
        ld_error("native image exceeds 16 MiB");
    ld_code_size = at + size;
    return at;
}

static struct ld_symbol *resolved(struct ld_symbol *s)
{
    return s->definition ? s->definition : s;
}

L64 ld_address(struct ld_symbol *symbol)
{
    struct ld_symbol *s = resolved(symbol);
    if (s->section == 65521U)
        return s->value;
    if (!s->section) {
        if (ld_kernel_mode && ld_kernel_special(s->name))
            return !strcmp(s->name, "kernel_end") ? ld_kernel_end : ld_kernel_readonly_end;
        if (s->helper < 0)
            ld_error("unresolved symbol '%s'", s->name);
        return trampolines + (L32)s->helper * 6;
    }
    if (!(s->object->sections[s->section].flags & 2))
        ld_error("reference to non-allocated symbol '%s'", s->name);
    return s->object->sections[s->section].address + s->value;
}

static void relocate(struct ld_object *o, struct ld_section *r)
{
    L32 i;
    struct ld_section *section = &o->sections[r->info];
    unsigned char *data = ld_read_relocations(o, r);
    for (i = 0; i < r->size; i += 24) {
        const unsigned char *p = data + i;
        L64 offset = ld_read(p, 8), info = ld_read(p + 8, 8), addend = ld_read(p + 16, 8);
        L32 type = (L32)(info & 0xffffffffULL), at, width = 0;
        L64 value, target;
        struct ld_symbol *s;
        int absolute;
        if ((info >> 32) >= o->symbol_count)
            ld_error("relocation has invalid symbol index");
        s = resolved(&o->symbols[(L32)(info >> 32)]);
        absolute = s->section == 65521U;
        if (type == 1)
            width = 8;
        else if (type == 2 || type == 4 || type == 10 || type == 11 ||
                 (ld_kernel_mode && (type == 41 || type == 42)))
            width = 4;
        else if (type == 12)
            width = 2;
        else if (type == 14)
            width = 1;
        else
            ld_error("unsupported x86_64 relocation %lu", type);
        if (offset > section->size || width > section->size - offset)
            ld_error("relocation outside target section");
        at = section->address + (L32)offset;
        if (!ld_kernel_mode)
            ld_modules_relocation(o, (unsigned)r->info, offset, (unsigned)width);
        {
            struct patch_span *patch = ld_append(&patches, sizeof *patch);
            patch->at = at;
            patch->width = width;
        }
        target = ld_address(s);
        if (type == 41 || type == 42) {
            /* Preserve GOT semantics for every instruction, including CMP.
             * Relaxation is optional and is not permission to rewrite an
             * arbitrary instruction into LEA. */
            target = ld_kernel_got(target, absolute);
            absolute = 0;
            type = 2;
        }
        value = target + addend; /* ELF addends are signed two's-complement. */
        if (type == 2 || type == 4) {
            if (absolute)
                ld_error("PC-relative reference to an absolute symbol is not load-independent");
            value -= at;
            if (value > 2147483647ULL && value < ~2147483647ULL)
                ld_error("PC-relative relocation overflow");
        } else if (type == 1 && !absolute) {
            struct ld_fixup *fix;
            /* RXE1 deliberately supports aligned in-image pointers only. */
            if ((!ld_kernel_mode && at % 8) ||
                value >= (ld_kernel_mode ? ld_kernel_end + 4096UL : ld_code_size))
                ld_error("unaligned/out-of-image native address relocation");
            fix = ld_append(&ld_fixups, sizeof *fix);
            fix->offset = at;
            fix->addend = (L32)value;
            value = 0;
        } else if (type != 1) {
            if (!absolute)
                ld_error(
                    "narrow absolute native address requires unsupported load-base relocation");
            if ((type == 11 && value > 2147483647ULL && value < ~2147483647ULL) ||
                (type != 11 && value > ((1ULL << (width * 8)) - 1)))
                ld_error("absolute relocation overflow");
        }
        ld_image_patch(at, value, (unsigned)width);
    }
    free(data);
}

static int compare_patch(const void *a, const void *b)
{
    L32 x = ((const struct patch_span *)a)->at, y = ((const struct patch_span *)b)->at;
    return x < y ? -1 : x > y;
}

void ld_link(void)
{
    size_t i, j;
    struct ld_object **objects = ld_objects.data;
    /* Stable helper ordering makes host/guest output byte-comparable. */
    for (i = 0; i < NH; i++)
        imports[i] = -1;
    for (i = 0; i < ld_objects.count; i++) {
        struct ld_object *o = objects[i];
        ld_path = o->path;
        for (j = 1; j < o->symbol_count; j++) {
            struct ld_symbol *s = &o->symbols[j];
            unsigned h;
            if (!s->binding || !s->section)
                continue;
            if (ld_kernel_mode && ld_kernel_special(s->name))
                ld_error("definition of reserved kernel layout symbol '%s'", s->name);
            if (!ld_kernel_mode && helper(s->name) >= 0)
                ld_error("definition of reserved runtime helper '%s'", s->name);
            if (find(s->name))
                ld_error("duplicate definition '%s'", s->name);
            h = hash(s->name);
            s->next = globals[h];
            globals[h] = s;
        }
    }
    for (i = 0; i < ld_objects.count; i++) {
        struct ld_object *o = objects[i];
        ld_path = o->path;
        for (j = 1; j < o->symbol_count; j++) {
            struct ld_symbol *s = &o->symbols[j];
            long h;
            if (s->section)
                continue;
            s->definition = find(s->name);
            if (s->definition)
                continue;
            if (ld_kernel_mode) {
                if (!ld_kernel_special(s->name))
                    ld_error("unresolved kernel symbol '%s'", s->name);
                continue;
            }
            h = helper(s->name);
            if (h < 0)
                ld_error("unresolved symbol '%s'", s->name);
            imports[h] = 0;
        }
    }
    for (i = 0; i < NH; i++)
        if (imports[i] == 0)
            imports[i] = (long)ld_helper_count++;
    for (i = 0; i < ld_objects.count; i++) {
        struct ld_object *o = objects[i];
        ld_path = o->path;
        for (j = 1; j < o->symbol_count; j++) {
            struct ld_symbol *s = &o->symbols[j];
            if (!ld_kernel_mode && !s->section && !s->definition)
                s->helper = imports[helper(s->name)];
        }
        for (j = 1; j < o->section_count; j++) {
            struct ld_section *s = &o->sections[j];
            if (!ld_kernel_mode && (s->flags & 2))
                s->address = reserve(s->alignment, s->size);
        }
    }
    if (ld_kernel_mode) {
        ld_kernel_layout();
        if (!find("_start") || !find("rcc_native_entry"))
            ld_error("kernel requires _start and rcc_native_entry definitions");
        ld_entry = (L32)ld_address(find("_start"));
    } else {
        trampolines = reserve(16, ld_helper_count * 6);
        ld_bindings = reserve(8, ld_helper_count * 8);
        reserve(1, 1);
    }
    ld_path = "final native image";
    ld_image_allocate();
    for (i = 0; i < ld_objects.count; i++) {
        struct ld_object *o = objects[i];
        ld_path = o->path;
        ld_copy_sections(o);
    }
    for (i = 0; i < ld_objects.count; i++) {
        struct ld_object *o = objects[i];
        ld_path = o->path;
        for (j = 1; j < o->section_count; j++) {
            struct ld_section *s = &o->sections[j];
            if (s->type == 4)
                relocate(o, s);
        }
    }
    for (i = 0; i < ld_helper_count; i++) {
        L32 at = trampolines + (L32)i * 6;
        ld_image_patch(at, 0x25ff, 2); /* jmp *bindings(%rip), preserving the ABI. */
        ld_image_patch(at + 2, (L64)ld_bindings + i * 8 - at - 6, 4);
    }
    if (patches.count > 1)
        qsort(patches.data, patches.count, sizeof(struct patch_span), compare_patch);
    for (i = 1; i < patches.count; i++) {
        struct patch_span *p = patches.data;
        if (p[i].at < p[i - 1].at + p[i - 1].width)
            ld_error("overlapping relocations");
    }
    for (i = 0; i < ld_functions.count; i++) {
        struct ld_function *f = &((struct ld_function *)ld_functions.data)[i];
        struct ld_symbol *s = f->symbol ? f->symbol : find(f->name);
        if (!s || s->section == 65521U || !(s->object->sections[s->section].flags & 4) ||
            s->value == s->object->sections[s->section].size)
            ld_error("continuation '%s' is not a defined executable symbol", f->name);
    }
    if (!ld_kernel_mode)
        ld_modules_patch();
}

static void number(FILE *f, L64 value, unsigned width)
{
    while (width--) {
        if (putc((int)(value & 255), f) == EOF)
            ld_error("cannot write executable");
        value >>= 8;
    }
}

void ld_write(FILE *f)
{
    L32 metadata = 64 + (L32)ld_functions.count * 16 + ld_helper_count * 72;
    L32 padding, i;
    size_t j;
    if (ld_fixups.count > (LD_LIMIT - metadata) / 16)
        ld_error("relocation metadata exceeds capacity");
    metadata += (L32)ld_fixups.count * 16;
    if (ld_initializers.count > (LD_LIMIT - metadata) / 24)
        ld_error("initializer metadata exceeds capacity");
    metadata += (L32)ld_initializers.count * 24;
    /* Package allocation is page aligned. Preserve every ELF alignment by
     * padding INSIDE the code blob; the loader ABI needs no change. */
    padding = (4096 - metadata % 4096) % 4096;
    if (metadata > LD_LIMIT || padding > LD_LIMIT - metadata ||
        ld_code_size > LD_LIMIT - metadata - padding)
        ld_error("executable exceeds 16 MiB package capacity");
    for (i = 0; i < 8; i++)
        number(f, (unsigned char)"FUSEX001"[i], 1);
    number(f, 0x31455852UL, 4);
    number(f, metadata + padding + ld_code_size, 4);
    number(f, padding + ld_code_size, 4);
    number(f, ld_functions.count, 4);
    number(f, ld_helper_count, 4);
    number(f, ld_fixups.count, 4);
    number(f, ld_initializers.count, 4);
    number(f, ld_entry, 4);
    number(f, ld_global_bank, 4);
    number(f, ld_global_cells, 4);
    number(f, 0, 4);
    number(f, ld_bindings + padding, 4);
    number(f, 0, 4);
    number(f, 0, 4);
    for (j = 0; j < ld_functions.count; j++) {
        struct ld_function *row = &((struct ld_function *)ld_functions.data)[j];
        number(f, row->token, 4);
        number(f, row->arity, 4);
        number(f, ld_address(row->symbol ? row->symbol : find(row->name)) + padding, 4);
        number(f, 0, 4);
    }
    for (j = 0; j < NH; j++)
        if (imports[j] >= 0) {
            size_t n = strlen(helpers[j].name);
            for (i = 0; i < 64; i++)
                number(f, i < n ? (unsigned char)helpers[j].name[i] : 0, 1);
            number(f, helpers[j].arity, 4);
            number(f, 0, 4);
        }
    for (j = 0; j < ld_fixups.count; j++) {
        struct ld_fixup *r = &((struct ld_fixup *)ld_fixups.data)[j];
        number(f, r->offset + padding, 8);
        number(f, r->addend + padding, 8);
    }
    for (j = 0; j < ld_initializers.count; j++) {
        struct ld_initializer *v = &((struct ld_initializer *)ld_initializers.data)[j];
        number(f, v->pointer, 4);
        number(f, v->width, 4);
        number(f, v->value, 8);
        number(f, v->function, 4);
        number(f, 0, 4);
    }
    for (i = 0; i < padding; i++)
        number(f, 0, 1);
    ld_image_write(f);
}
