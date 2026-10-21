/* All guest-visible addresses remain logical C16 pointers. QBE owns only
 * native registers/spills; opaque runtime helpers own memory and resumptions. */
#include "native.h"
#include <stdlib.h>
#include <string.h>
#define P(...) fprintf(o->file, __VA_ARGS__)

void native_instruction(struct native_output *o, struct instruction *i)
{
    static const char *names[] = {"add", "sub", "mul", "sdiv", "udiv", "srem", "urem", "and",
                                  "or",  "xor", "shl", "lshr", "ashr", "eq",   "ne",   "slt",
                                  "sle", "sgt", "sge", "ult",  "ule",  "ugt",  "uge"};
    static const char *qbe[] = {"add",   "sub",   "mul",   "div",   "udiv",  "rem",
                                "urem",  "and",   "or",    "xor",   "shl",   "shr",
                                "sar",   "ceql",  "cnel",  "csltl", "cslel", "csgtl",
                                "csgel", "cultl", "culel", "cugtl", "cugel"};
    char *a, *b, *r;
    size_t n;
    unsigned width;
    if (!strcmp(i->op, "intrinsic")) {
        if (!native_varargs(o, i))
            native_float(o, i);
        return;
    }
    if (!strcmp(i->op, "alloca")) {
        r = native_temporary(o);
        P("    %s =l call $rcc_native_gep(l %%ctx, l %%frame, l %lu)\n", r, i->object);
        native_assign(o, i, r);
        return;
    }
    if (!strcmp(i->op, "load") || !strcmp(i->op, "store")) {
        if (!i->type->bits || i->type->size > 4)
            fail("native aggregate memory operation is not implemented");
        b = native_value(o, i->b);
        if (!strcmp(i->op, "load")) {
            r = native_temporary(o);
            P("    %s =l call $rcc_native_load(l %%ctx, l %s, l %lu)\n", r, b, i->type->size);
            native_assign(o, i, r);
        } else {
            a = native_mask(o, native_value(o, i->a), i->type->bits);
            P("    call $rcc_native_store(l %%ctx, l %s, l %s, l %lu)\n", b, a, i->type->size);
        }
        return;
    }
    if (!strcmp(i->op, "getelementptr")) {
        struct value **args = i->args.data;
        if (i->args.count != 1 || i->source_type->size != 1)
            fail("native feasibility GEP requires one cell offset");
        a = native_value(o, i->a);
        b = native_value(o, args[0]);
        if (args[0]->kind != VNUMBER) {
            unsigned bits = native_operand_bits(o, args[0]);
            b = bits <= 16 ? native_mask(o, b, bits) : native_sign(o, b, bits);
        }
        r = native_temporary(o);
        P("    %s =l call $rcc_native_gep(l %%ctx, l %s, l %s)\n", r, a, b);
        native_assign(o, i, r);
        return;
    }
    a = native_value(o, i->a);
    if (!strcmp(i->op, "ptrtolinear")) {
        char *bank = native_temporary(o), *masked = native_temporary(o);
        char *cells = native_temporary(o), *offset = native_temporary(o);
        r = native_temporary(o);
        P("    %s =l shr %s, 16\n    %s =l and %s, 65535\n", bank, a, masked, bank);
        P("    %s =l mul %s, 64000\n    %s =l and %s, 65535\n", cells, masked, offset, a);
        P("    %s =l add %s, %s\n", r, cells, offset);
        native_assign(o, i, r);
        return;
    }
    if (!strcmp(i->op, "inttoptr")) {
        /* Native pointers are logical 32-bit carriers. Preserve their bits;
         * dereference/GEP helpers still validate canonical addresses. In
         * particular, integer zero must compare as a null pointer. */
        if (i->type->kind != TYPOINTER)
            fail("native inttoptr requires a pointer result");
        native_assign(o, i, a);
        return;
    }
    if (!strcmp(i->op, "select")) {
        unsigned long tag = ++o->temporary;
        char *condition = native_temporary(o), *yes = native_value(o, i->b),
             *no = native_value(o, i->c);
        P("    %s =w cnel %s, 0\n    jnz %s, @select%lu_yes, @select%lu_no\n", condition, a,
          condition, tag, tag);
        P("@select%lu_yes\n", tag);
        P("    %%v%ld =l copy %s\n    jmp @select%lu_end\n", i->result,
          native_mask(o, yes, i->type->bits), tag);
        P("@select%lu_no\n", tag);
        P("    %%v%ld =l copy %s\n    jmp @select%lu_end\n@select%lu_end\n", i->result,
          native_mask(o, no, i->type->bits), tag, tag);
        return;
    }
    if (!strcmp(i->op, "sext")) {
        native_assign(o, i, native_sign(o, a, native_operand_bits(o, i->a)));
        return;
    }
    if (!strcmp(i->op, "zext") || !strcmp(i->op, "trunc") || !strcmp(i->op, "ptrtoint")) {
        native_assign(o, i, native_mask(o, a, native_operand_bits(o, i->a)));
        return;
    }
    if (!strcmp(i->op, "neg") || !strcmp(i->op, "not")) {
        r = native_temporary(o);
        P("    %s =l %s %s, %s\n", r, !strcmp(i->op, "neg") ? "sub" : "xor",
          !strcmp(i->op, "neg") ? "0" : a, !strcmp(i->op, "neg") ? a : "18446744073709551615");
        native_assign(o, i, r);
        return;
    }
    for (n = 0; n < sizeof names / sizeof names[0]; n++)
        if (!strcmp(i->op, names[n]))
            break;
    if (n == sizeof names / sizeof names[0])
        fail("unsupported native operation '%s'", i->op);
    {
        struct type *left = native_value_type(o->function, i->a);
        struct type *right = native_value_type(o->function, i->b);
        if (i->type->kind != TYINT || (left && left->kind == TYFLOAT) ||
            (right && right->kind == TYFLOAT))
            fail("native integer operation has non-integer operands/result");
    }
    b = native_value(o, i->b);
    width = i->type->bits;
    if (n >= 13) {
        if (i->a->kind == VNUMBER && i->b->kind == VNUMBER)
            width = 64;
        else if (i->a->kind == VNUMBER)
            width = native_operand_bits(o, i->b);
        else if (i->b->kind == VNUMBER)
            width = native_operand_bits(o, i->a);
        else {
            unsigned other = native_operand_bits(o, i->b);
            width = native_operand_bits(o, i->a);
            if (other > width)
                width = other;
        }
    }
    if (n == 3 || n == 5 || n == 12 || (n >= 15 && n <= 18)) {
        a = native_sign(o, a, width);
        if (n != 12)
            b = native_sign(o, b, width);
    } else {
        a = native_mask(o, a, width);
        if (n != 10 && n != 11)
            b = native_mask(o, b, width);
    }
    if (n >= 10 && n <= 12) {
        char *count = native_temporary(o);
        P("    %s =l and %s, %u\n", count, b, width - 1);
        b = count;
    }
    if (n >= 3 && n <= 6) {
        char *zero = native_temporary(o), *z = native_temporary(o);
        char *safe = native_temporary(o), *quotient = native_temporary(o);
        char *keep = native_temporary(o);
        /* Match RCC's defined scalar behavior: division by zero returns zero,
         * and signed MIN/-1 wraps. Never execute a trapping host division. */
        P("    %s =w ceql %s, 0\n    %s =l extuw %s\n    %s =l or %s, %s\n", zero, b, z, zero, safe,
          b, z);
        if ((n == 3 || n == 5) && width == 64) {
            char *min = native_temporary(o), *minus = native_temporary(o);
            char *ov = native_temporary(o), *wide = native_temporary(o);
            char *mask = native_temporary(o), *inverse = native_temporary(o);
            char *partial = native_temporary(o), *divisor = native_temporary(o);
            P("    %s =w ceql %s, 9223372036854775808\n    %s =w ceql %s, 18446744073709551615\n",
              min, a, minus, b);
            P("    %s =w and %s, %s\n    %s =l extuw %s\n    %s =l sub 0, %s\n", ov, min, minus,
              wide, ov, mask, wide);
            P("    %s =l xor %s, 18446744073709551615\n    %s =l and %s, %s\n    %s =l or %s, %s\n",
              inverse, mask, partial, safe, inverse, divisor, partial, wide);
            safe = divisor;
        }
        P("    %s =l %s %s, %s\n    %s =l sub %s, 1\n", quotient, qbe[n], a, safe, keep, z);
        r = native_temporary(o);
        P("    %s =l and %s, %s\n", r, quotient, keep);
        native_assign(o, i, r);
        return;
    }
    r = native_temporary(o);
    P("    %s =%c %s %s, %s\n", r, n >= 13 ? 'w' : 'l', qbe[n], a, b);
    if (n >= 13) {
        char *extended = native_temporary(o);
        P("    %s =l extuw %s\n", extended, r);
        r = extended;
    }
    native_assign(o, i, r);
}
