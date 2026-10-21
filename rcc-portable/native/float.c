/* Typed floating operations become SSE2 through QBE. Boundary values retain
 * IEEE integer carriers; convert.c handles saturating numeric conversions. */
#include "native.h"
#include <string.h>
#define P(...) fprintf(o->file, __VA_ARGS__)

char *native_as_float(struct native_output *o, char *value, unsigned bits)
{
    char *result = native_temporary(o);
    if (bits == 32) {
        char *word = native_temporary(o);
        P("    %s =w copy %s\n    %s =s cast %s\n", word, value, result, word);
    } else
        P("    %s =d cast %s\n", result, value);
    return result;
}

void native_float(struct native_output *o, struct instruction *i)
{
    const char *name = i->a->name, *operation;
    struct value **args = i->args.data;
    unsigned bits;
    char precision, *a, *b, *r, *carrier;
    int compare;
    if (strncmp(name, "float.", 6))
        fail("unsupported native intrinsic '%s'", name);
    if (!strncmp(name, "float.value.", 12) || !strncmp(name, "float.bits.", 11)) {
        int typed = !strncmp(name, "float.value.", 12);
        const char *suffix = name + (typed ? 12 : 11);
        struct type *source;
        bits = !strcmp(suffix, "f32") ? 32 : !strcmp(suffix, "f64") ? 64 : 0;
        if (!bits || i->args.count != 1 || i->type->bits != bits ||
            i->type->kind != (typed ? TYFLOAT : TYINT))
            fail("invalid floating reinterpret signature");
        source = native_value_type(o->function, args[0]);
        if (source && (source->bits != bits || source->kind != (typed ? TYINT : TYFLOAT)))
            fail("floating reinterpret operand type mismatch");
        a = native_mask(o, native_value(o, args[0]), bits);
        P("    %%v%ld =l copy %s\n", i->result, a);
        return;
    }
    bits = !strncmp(name, "float.f32.", 10) ? 32 : !strncmp(name, "float.f64.", 10) ? 64 : 0;
    if (!bits)
        fail("unsupported native floating operation '%s'", name);
    operation = name + 10;
    precision = bits == 32 ? 's' : 'd';
    if (native_conversion(o, i, bits, operation))
        return;
    if ((bits == 64 && !strcmp(operation, "from_f32")) ||
        (bits == 32 && !strcmp(operation, "from_f64"))) {
        unsigned source_bits = bits == 64 ? 32 : 64;
        struct type *source;
        if (i->args.count != 1 || i->result < 0 || i->type->kind != TYFLOAT ||
            i->type->bits != bits)
            fail("invalid floating precision conversion signature");
        source = native_value_type(o->function, args[0]);
        if (source && (source->kind != TYFLOAT || source->bits != source_bits))
            fail("floating precision conversion operand mismatch");
        a = native_as_float(o, native_value(o, args[0]), source_bits);
        r = native_temporary(o);
        carrier = native_temporary(o);
        P("    %s =%c %s %s\n", r, precision, bits == 64 ? "exts" : "truncd", a);
        if (bits == 64)
            P("    %s =l cast %s\n", carrier, r);
        else {
            char *word = native_temporary(o);
            P("    %s =w cast %s\n    %s =l extuw %s\n", word, r, carrier, word);
        }
        native_assign(o, i, carrier);
        return;
    }
    compare = !strcmp(operation, "eq") || !strcmp(operation, "ne") || !strcmp(operation, "lt") ||
              !strcmp(operation, "le") || !strcmp(operation, "gt") || !strcmp(operation, "ge");
    if (!compare && strcmp(operation, "add") && strcmp(operation, "sub") &&
        strcmp(operation, "mul") && strcmp(operation, "div"))
        fail("native feasibility floating conversion '%s' is not implemented", name);
    if (i->args.count != 2 || i->type->kind != (compare ? TYINT : TYFLOAT) ||
        (!compare && i->type->bits != bits))
        fail("invalid native floating signature");
    {
        size_t n;
        for (n = 0; n < 2; n++) {
            struct type *source = native_value_type(o->function, args[n]);
            if (source && (source->bits != bits || source->kind != TYFLOAT))
                fail("floating operand type mismatch");
        }
    }
    a = native_as_float(o, native_value(o, args[0]), bits);
    b = native_as_float(o, native_value(o, args[1]), bits);
    r = native_temporary(o);
    carrier = native_temporary(o);
    if (compare) {
        P("    %s =w c%s%c %s, %s\n    %s =l extuw %s\n", r, operation, precision, a, b, carrier,
          r);
    } else {
        P("    %s =%c %s %s, %s\n", r, precision, operation, a, b);
        if (bits == 32) {
            char *word = native_temporary(o);
            P("    %s =w cast %s\n    %s =l extuw %s\n", word, r, carrier, word);
        } else
            P("    %s =l cast %s\n", carrier, r);
    }
    P("    %%v%ld =l copy %s\n", i->result, carrier);
}
