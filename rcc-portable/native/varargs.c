/* C16 va_list is a logical pointer to contiguous, unaligned cell carriers.
 * The caller's persistent frame owns the area across all suspensions. */
#include "native.h"
#include <string.h>
#define P(...) fprintf(o->file, __VA_ARGS__)

int native_varargs(struct native_output *o, struct instruction *i)
{
    const char *name = i->a->name;
    struct value **args = i->args.data;
    char *ap, *cursor;
    if (strcmp(name, "va_start") && strcmp(name, "va_arg"))
        return 0;
    if (i->args.count != 1)
        fail("native %s requires one va_list address", name);
    ap = native_value(o, args[0]);
    cursor = native_temporary(o);
    if (!strcmp(name, "va_start")) {
        if (!o->function->function->vararg || i->result >= 0 || i->type->kind != TYVOID)
            fail("invalid native va_start/outside variadic definition");
        P("    %s =l call $rcc_native_process_arg(l %%ctx, l %lu)\n", cursor,
          (unsigned long)o->function->function->parameters.count);
        P("    call $rcc_native_store(l %%ctx, l %s, l %s, l 2)\n", ap, cursor);
    } else {
        char *value = native_temporary(o), *next = native_temporary(o);
        if (i->result < 0 || !i->type->bits || !i->type->size || i->type->size > 4)
            fail("native va_arg requires a scalar result");
        P("    %s =l call $rcc_native_load(l %%ctx, l %s, l 2)\n", cursor, ap);
        P("    %s =l call $rcc_native_load(l %%ctx, l %s, l %lu)\n", value, cursor, i->type->size);
        P("    %s =l call $rcc_native_gep(l %%ctx, l %s, l %lu)\n", next, cursor, i->type->size);
        P("    call $rcc_native_store(l %%ctx, l %s, l %s, l 2)\n", ap, next);
        native_assign(o, i, value);
    }
    return 1;
}
