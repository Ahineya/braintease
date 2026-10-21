/* All guest-visible addresses remain logical C16 pointers. QBE owns only
 * native registers/spills; opaque runtime helpers own memory and resumptions. */
#include "native.h"
#include <stdlib.h>
#include <string.h>
#define P(...) fprintf(o->file, __VA_ARGS__)

char *native_temporary(struct native_output *o)
{
    char text[40];
    snprintf(text, sizeof text, "%%t%lu", ++o->temporary);
    return native_copy(text);
}

char *native_mask(struct native_output *o, const char *value, unsigned bits)
{
    char *result;
    if (bits == 64)
        return native_copy(value);
    if (!bits || bits > 64)
        fail("invalid native scalar width");
    result = native_temporary(o);
    P("    %s =l and %s, %llu\n", result, value, (1ULL << bits) - 1);
    return result;
}

char *native_sign(struct native_output *o, const char *value, unsigned bits)
{
    char *left, *right;
    if (bits == 64)
        return native_copy(value);
    if (!bits || bits > 64)
        fail("invalid native signed width");
    left = native_temporary(o);
    right = native_temporary(o);
    P("    %s =l shl %s, %u\n    %s =l sar %s, %u\n", left, value, 64 - bits, right, left,
      64 - bits);
    return right;
}

struct function *native_find_function(struct module *m, const char *name)
{
    struct function *functions = m->functions.data;
    size_t n;
    for (n = 0; n < m->functions.count; n++)
        if (!functions[n].external && !strcmp(functions[n].name, name))
            return &functions[n];
    return NULL;
}

static unsigned long global_address(struct module *m, const char *name)
{
    struct symbol *symbols = m->symbols.data;
    unsigned long offset = 1;
    size_t n;
    for (n = 0; n < m->symbols.count; n++)
        if (!symbols[n].function && symbols[n].defined) {
            if (!strcmp(symbols[n].name, name))
                return ((1 + offset / 64000UL) << 16) | (offset % 64000UL);
            if (symbols[n].type->size > 63999UL - offset)
                fail("native feasibility globals exceed one bank");
            offset += symbols[n].type->size;
        }
    fail("undefined native global '%s'", name);
    return 0;
}

int native_global_reference(struct value *v)
{
    while (v && v->kind == VOFFSET)
        v = v->base;
    return v && v->kind == VGLOBAL;
}

char *native_value(struct native_output *o, struct value *v)
{
    char text[64], *result, *base;
    struct function *function;
    if (!v)
        fail("missing native IR operand");
    switch (v->kind) {
    case VNUMBER:
        snprintf(text, sizeof text, "%llu", v->bits);
        return native_copy(text);
    case VTEMP:
        native_variable(o->function, v->id);
        snprintf(text, sizeof text, "%%v%lu", v->id);
        return native_copy(text);
    case VGLOBAL:
        if (o->relocatable)
            return native_binding_value(o, NM_GLOBAL, v->name);
        snprintf(text, sizeof text, "%lu", global_address(o->module, v->name));
        return native_copy(text);
    case VFUNCTION:
        if (o->kernel)
            return native_binding_value(o, NM_FUNCTION, v->name);
        if (o->relocatable) {
            char *local = native_binding_value(o, NM_FUNCTION, v->name);
            result = native_temporary(o);
            P("    %s =l call $rcc_native_image_token(l %%ctx, l %s)\n", result, local);
            return result;
        }
        function = native_find_function(o->module, v->name);
        if (!function)
            fail("native external function token '%s' is not supported", v->name);
        result = native_temporary(o);
        P("    %s =l call $rcc_native_image_token(l %%ctx, l %lu)\n", result,
          0x80000000UL | function->serial);
        return result;
    case VFAT:
        if (v->bank == BNULL)
            return native_copy("0");
        if (o->relocatable && v->bank == BGLOBAL && native_global_reference(v->base))
            return native_value(o, v->base);
        if (v->bank == BMIXED)
            return native_value(o, v->base);
        if (v->base->kind != VNUMBER || v->base->bits >= 64000 || v->bank == BSTACK)
            fail("native feasibility backend requires resolved pointer tags");
        snprintf(text, sizeof text, "%lu",
                 ((v->bank == BGLOBAL ? 1UL : (unsigned long)v->bank) << 16) |
                     (unsigned long)v->base->bits);
        return native_copy(text);
    case VOFFSET:
        base = native_value(o, v->base);
        result = native_temporary(o);
        P("    %s =l call $rcc_native_gep(l %%ctx, l %s, l %ld)\n", result, base, v->offset);
        return result;
    default:
        fail("unsupported native value kind %d", v->kind);
    }
    return NULL;
}

void native_assign(struct native_output *o, struct instruction *i, const char *value)
{
    if (i->result < 0)
        fail("native value-producing instruction has no result");
    P("    %%v%ld =l copy %s\n", i->result, native_mask(o, value, i->type->bits));
}

unsigned native_operand_bits(struct native_output *o, struct value *value)
{
    struct type *type = native_value_type(o->function, value);
    if (value->kind == VFAT || value->kind == VGLOBAL || value->kind == VFUNCTION ||
        value->kind == VOFFSET)
        return 32;
    return type ? type->bits : 16;
}
