/* All guest-visible addresses remain logical C16 pointers. QBE owns only
 * native registers/spills; opaque runtime helpers own memory and resumptions. */
#include "native.h"
#include <stdlib.h>
#include <string.h>
#define P(...) fprintf(o->file, __VA_ARGS__)

static void continuation(struct native_output *o, unsigned long pc, unsigned char *live,
                         int restore, long skip)
{
    struct native_var *vars = o->function->variables.data;
    unsigned long slot = 0;
    size_t n;
    if (!restore)
        P("    call $rcc_native_process_set_pc(l %%ctx, l %lu)\n", pc);
    for (n = 0; n < o->function->variables.count; n++)
        if (live[n]) {
            if (vars[n].parameter >= 0) {
                if (restore) {
                    char *raw = native_temporary(o);
                    P("    %s =l call $rcc_native_process_arg(l %%ctx, l %ld)\n", raw,
                      vars[n].parameter);
                    P("    %%v%lu =l copy %s\n", vars[n].id,
                      native_mask(o, raw, vars[n].type->bits));
                }
            } else {
                if (slot >= 512)
                    fail("native continuation exceeds 512 live slots");
                if (restore) {
                    char *raw = native_temporary(o);
                    P("    %s =l call $rcc_native_process_slot(l %%ctx, l %lu)\n", raw, slot);
                    P("    %%v%lu =l copy %s\n", vars[n].id,
                      native_mask(o, raw, vars[n].type->bits));
                } else if ((long)vars[n].id != skip)
                    P("    call $rcc_native_process_save(l %%ctx, l %lu, l %%v%lu)\n", slot,
                      vars[n].id);
                slot++;
            }
        }
}

void native_call(struct native_output *o, struct native_point *point)
{
    struct instruction *i = point->instruction;
    struct value **args = i->args.data;
    struct native_var *vars = o->function->variables.data;
    unsigned long slot = 0, result = 0;
    char **values = native_scratch(i->args.count * sizeof(*values));
    char *token;
    size_t n, count = i->args.count;
    int variadic = i->stack_from != 0xffffffffUL;
    int service = i->a->kind == VFUNCTION ? native_service(o->module, i->a->name) : 0;
    int imported = 0;
    unsigned arity = 1;
    if (o->kernel && i->a->kind == VFUNCTION && !native_find_function(o->module, i->a->name)) {
        service = native_process_service(i->a->name, &arity);
        if (!service)
            imported = native_kernel_import(o->module, i->a->name);
        if (!service && !imported)
            fail("unknown native kernel service '%s'", i->a->name);
    }
    if (!variadic && count > 64)
        fail("native call exceeds 64 arguments");
    if (i->a->kind == VFUNCTION) {
        struct function *callee = o->relocatable ? native_declaration(o->module, i->a->name)
                                                 : native_find_function(o->module, i->a->name);
        if (!callee || callee->vararg != variadic ||
            (variadic ? callee->parameters.count != i->stack_from || count < i->stack_from
                      : callee->parameters.count != count))
            fail("native callee is undefined or has incompatible arity");
        if (callee->result->kind != i->type->kind || callee->result->bits != i->type->bits)
            fail("native call result type disagrees with its callee");
    }
    if (service && (variadic || count != arity))
        fail("invalid native process service signature");
    if (service && !o->kernel && (i->type->kind != TYINT || i->type->bits != 32))
        fail("invalid native syscall service signature");
    if (imported && (variadic || count != (unsigned)(imported - 1)))
        fail("invalid native kernel service signature");
    token = service || imported ? NULL : native_value(o, i->a);
    for (n = 0; n < i->args.count; n++)
        values[n] = native_value(o, args[n]);
    if (imported) {
        char *raw = native_temporary(o);
        P("    jmp @resume%lu\n@resume%lu\n    %s =l call $rcc_native_ext_%s(l %%ctx",
          point->call_pc, point->call_pc, raw, i->a->name);
        for (n = 0; n < count; n++)
            P(", l %s", values[n]);
        P(")\n");
        if (i->result >= 0)
            native_assign(o, i, raw);
        return;
    }
    if (variadic) {
        char *base = native_temporary(o), *cursor;
        P("    %s =l call $rcc_native_gep(l %%ctx, l %%frame, l %lu)\n", base,
          o->function->var_area);
        cursor = base;
        for (n = i->stack_from; n < count; n++) {
            unsigned cells = native_argument_cells(o->function, args[n]);
            char *next = native_temporary(o);
            P("    call $rcc_native_store(l %%ctx, l %s, l %s, l %u)\n", cursor, values[n], cells);
            P("    %s =l call $rcc_native_gep(l %%ctx, l %s, l %u)\n", next, cursor, cells);
            cursor = next;
        }
        count = i->stack_from + 1;
        /* With no tail arguments there was no space in values for the hidden
         * cursor; allocate a separate vector valid for either case. */
        {
            char **packed = native_scratch(count * sizeof *packed);
            for (n = 0; n < i->stack_from; n++)
                packed[n] = values[n];
            packed[i->stack_from] = base;
            values = packed;
        }
    }
    for (n = 0; n < o->function->variables.count; n++)
        if (point->after[n] && vars[n].parameter < 0) {
            if ((long)vars[n].id == i->result)
                result = slot + 1;
            slot++;
        }
    continuation(o, point->call_pc, point->after, 0, i->result);
    if (service)
        P("    call $rcc_native_process_request(l %%ctx, l %d, l %s, l %s, l %lu)\n    ret 0\n",
          service, count ? values[0] : "0", count > 1 ? values[1] : "0", result);
    else {
        for (n = 0; n < count; n++)
            P("    call $rcc_native_process_argument(l %%ctx, l %lu, l %s)\n", (unsigned long)n,
              values[n]);
        P("    call $rcc_native_process_call(l %%ctx, l %s, l %lu, l %lu)\n    ret 0\n", token,
          (unsigned long)count, result);
    }
    P("@resume%lu\n", point->call_pc);
    continuation(o, point->call_pc, point->after, 1, -1);
}

static void branch_edge(struct native_output *o, unsigned long from, unsigned long to,
                        const char *label)
{
    struct native_block *blocks = o->function->blocks.data,
                        *target = &blocks[native_block(o->function, to)];
    struct native_point *points = target->points.data;
    struct vec staged = {0};
    size_t n, k;
    P("%s\n", label);
    for (n = 0; n < target->points.count; n++) {
        struct instruction *phi = points[n].instruction;
        unsigned long *labels = phi->labels.data;
        struct value **args = phi->args.data;
        char *source, *saved;
        if (strcmp(phi->op, "phi"))
            break;
        for (k = 0; k < phi->labels.count; k++)
            if (labels[k] == from)
                break;
        if (k == phi->labels.count)
            fail("missing phi predecessor");
        source = native_value(o, args[k]);
        saved = native_temporary(o);
        P("    %s =l copy %s\n", saved, source);
        *(char **)append(&staged, sizeof(char *)) = saved;
    }
    for (n = 0; n < staged.count; n++)
        native_assign(o, points[n].instruction, ((char **)staged.data)[n]);
    P("    jmp @block%lu\n", to);
    free(staged.data);
}

void native_branch(struct native_output *o, struct native_block *block, struct native_point *point)
{
    struct instruction *i = point->instruction;
    char yes[64], no[64], *condition;
    if (point->checkpoint_pc) {
        unsigned long pc = point->checkpoint_pc;
        unsigned char *live = native_scratch(o->function->variables.count);
        char *due = native_temporary(o);
        memcpy(live, point->after, o->function->variables.count);
        native_before(o->function, i, live);
        P("    %s =l call $rcc_native_process_checkpoint(l %%ctx)\n    jnz %s, @suspend%lu, "
          "@proceed%lu\n@suspend%lu\n",
          due, due, pc, pc, pc);
        continuation(o, pc, live, 0, -1);
        P("    call $rcc_native_process_request(l %%ctx, l 15, l 0, l 0, l 0)\n    ret "
          "0\n@resume%lu\n",
          pc);
        continuation(o, pc, live, 1, -1);
        P("    jmp @proceed%lu\n@proceed%lu\n", pc, pc);
    }
    P("    call $rcc_native_poll(l %%ctx)\n");
    snprintf(yes, sizeof yes, "@edge%lu_%lu_y", block->block->id, i->yes);
    snprintf(no, sizeof no, "@edge%lu_%lu_n", block->block->id, i->no);
    if (i->conditional) {
        char *value = native_value(o, i->a);
        condition = native_temporary(o);
        P("    %s =w cnel %s, 0\n    jnz %s, %s, %s\n", condition, value, condition, yes, no);
    } else
        P("    jmp %s\n", yes);
    branch_edge(o, block->block->id, i->yes, yes);
    if (i->conditional)
        branch_edge(o, block->block->id, i->no, no);
}
