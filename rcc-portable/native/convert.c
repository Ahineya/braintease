/* Numeric conversions preserve RCC's saturating native contract. QBE's
 * float-to-integer instruction is reached only for a finite in-range operand. */
#include "native.h"
#include <string.h>
#define P(...) fprintf(o->file, __VA_ARGS__)

static char *power(struct native_output *o, unsigned precision, unsigned exponent, int negative)
{
    char text[32];
    unsigned long long bits = precision == 32 ? (unsigned long long)(127 + exponent) << 23
                                              : (unsigned long long)(1023 + exponent) << 52;
    if (negative)
        bits |= 1ULL << (precision - 1);
    snprintf(text, sizeof text, "%llu", bits);
    return native_as_float(o, native_copy(text), precision);
}

int native_conversion(struct native_output *o, struct instruction *i, unsigned precision,
                      const char *operation)
{
    struct value **args = i->args.data;
    struct type *source;
    unsigned width;
    int from, signed_value;
    char *input, *result, *carrier;
    char kind = precision == 32 ? 's' : 'd';
    if (!strcmp(operation, "from_si") || !strcmp(operation, "from_ui") ||
        !strcmp(operation, "from_i64") || !strcmp(operation, "from_u64"))
        from = 1;
    else if (!strcmp(operation, "to_si") || !strcmp(operation, "to_ui") ||
             !strcmp(operation, "to_i64") || !strcmp(operation, "to_u64"))
        from = 0;
    else
        return 0;
    width = strstr(operation, "64") ? 64 : 32;
    signed_value = operation[from ? 5 : 3] != 'u';
    if (i->args.count != 1 || i->result < 0 || i->type->kind != (from ? TYFLOAT : TYINT) ||
        i->type->bits != (from ? precision : width))
        fail("invalid numeric floating conversion signature");
    source = native_value_type(o->function, args[0]);
    if (source &&
        (source->kind != (from ? TYINT : TYFLOAT) || source->bits != (from ? width : precision)))
        fail("numeric floating conversion operand mismatch");
    input = native_value(o, args[0]);
    result = native_temporary(o);
    if (from) {
        input = signed_value ? native_sign(o, input, width) : native_mask(o, input, width);
        P("    %s =%c %s %s\n", result, kind, signed_value ? "sltof" : "ultof", input);
        carrier = native_temporary(o);
        if (precision == 64)
            P("    %s =l cast %s\n", carrier, result);
        else {
            char *word = native_temporary(o);
            P("    %s =w cast %s\n    %s =l extuw %s\n", word, result, carrier, word);
        }
    } else {
        unsigned long tag = ++o->temporary;
        unsigned long long low = signed_value ? 1ULL << (width - 1) : 0;
        unsigned long long high = signed_value ? low - 1 : width == 64 ? ~0ULL : 0xffffffffULL;
        char *unordered = native_temporary(o), *below = native_temporary(o);
        char *above = native_temporary(o), *lower, *upper;
        input = native_as_float(o, input, precision);
        lower =
            signed_value ? power(o, precision, width - 1, 1) : native_as_float(o, "0", precision);
        upper = power(o, precision, width - (signed_value != 0), 0);
        P("    %s =w cne%c %s, %s\n    jnz %s, @convert%lu_nan, @convert%lu_low\n", unordered, kind,
          input, input, unordered, tag, tag);
        P("@convert%lu_nan\n    %s =l copy 0\n    jmp @convert%lu_end\n", tag, result, tag);
        P("@convert%lu_low\n    %s =w cle%c %s, %s\n"
          "    jnz %s, @convert%lu_min, @convert%lu_high\n",
          tag, below, kind, input, lower, below, tag, tag);
        P("@convert%lu_min\n    %s =l copy %llu\n    jmp @convert%lu_end\n", tag, result, low, tag);
        P("@convert%lu_high\n    %s =w cge%c %s, %s\n"
          "    jnz %s, @convert%lu_max, @convert%lu_finite\n",
          tag, above, kind, input, upper, above, tag, tag);
        P("@convert%lu_max\n    %s =l copy %llu\n    jmp @convert%lu_end\n", tag, result, high,
          tag);
        P("@convert%lu_finite\n", tag);
        if (!signed_value && width == 64) {
            char *split = power(o, precision, 63, 0), *large = native_temporary(o);
            char *reduced = native_temporary(o), *integer = native_temporary(o);
            P("    %s =w cge%c %s, %s\n    jnz %s, @convert%lu_upper, @convert%lu_lower\n", large,
              kind, input, split, large, tag, tag);
            P("@convert%lu_upper\n    %s =%c sub %s, %s\n    %s =l %ctosi %s\n"
              "    %s =l or %s, 9223372036854775808\n    jmp @convert%lu_end\n",
              tag, reduced, kind, input, split, integer, kind, reduced, result, integer, tag);
            P("@convert%lu_lower\n", tag);
        }
        P("    %s =l %ctosi %s\n    jmp @convert%lu_end\n@convert%lu_end\n", result, kind, input,
          tag, tag);
        carrier = result;
    }
    native_assign(o, i, carrier);
    return 1;
}
