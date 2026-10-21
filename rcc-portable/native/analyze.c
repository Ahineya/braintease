/* Compact liveness follows logical CFG edges, including phi edge uses. Only
 * live non-parameters cross suspension; parameters stay in immutable args. */
#include "native.h"
#include <string.h>

long native_variable(struct native_function *f, unsigned long id)
{
    struct native_var *v = f->variables.data;
    size_t n;
    for (n = 0; n < f->variables.count; n++)
        if (v[n].id == id)
            return (long)n;
    fail("undefined native IR temporary %%%lu", id);
    return -1;
}

long native_block(struct native_function *f, unsigned long id)
{
    struct native_block *b = f->blocks.data;
    size_t n;
    for (n = 0; n < f->blocks.count; n++)
        if (b[n].block->id == id)
            return (long)n;
    fail("undefined native IR block %lu", id);
    return -1;
}

struct type *native_value_type(struct native_function *f, struct value *v)
{
    if (v && v->kind == VTEMP) {
        struct native_var *vars = f->variables.data;
        return vars[native_variable(f, v->id)].type;
    }
    if (v && (v->kind == VFAT || v->kind == VOFFSET))
        return native_value_type(f, v->base);
    return NULL;
}

unsigned native_argument_cells(struct native_function *f, struct value *v)
{
    struct type *t = native_value_type(f, v);
    if (v->kind == VFAT || v->kind == VOFFSET || v->kind == VGLOBAL || v->kind == VFUNCTION)
        return 2;
    if (t && (!t->bits || !t->size || t->size > 4))
        fail("variadic argument requires a scalar carrier");
    /* Untyped IR constants have C16 int width, matching the existing native
     * backend. The frontend emits typed SSA casts for wider C constants. */
    return t ? (unsigned)t->size : 1;
}

static void use(struct native_function *f, struct value *v, unsigned char *live)
{
    if (!v)
        return;
    if (v->kind == VTEMP)
        live[native_variable(f, v->id)] = 1;
    else if (v->kind == VFAT || v->kind == VOFFSET)
        use(f, v->base, live);
}

void native_before(struct native_function *f, struct instruction *i, unsigned char *live)
{
    struct value **args = i->args.data;
    size_t n;
    if (i->result >= 0)
        live[native_variable(f, (unsigned long)i->result)] = 0;
    if (!strcmp(i->op, "phi"))
        return;
    use(f, i->a, live);
    use(f, i->b, live);
    use(f, i->c, live);
    for (n = 0; n < i->args.count; n++)
        use(f, args[n], live);
}

static void variable(struct native_function *f, unsigned long id, struct type *type, long parameter)
{
    struct native_var *v = f->variables.data;
    size_t n;
    for (n = 0; n < f->variables.count; n++)
        if (v[n].id == id)
            fail("duplicate native IR temporary %%%lu", id);
    if (!type || !type->bits || type->kind == TYARRAY || type->kind == TYSTRUCT)
        fail("native feasibility backend requires scalar SSA values");
    v = append(&f->variables, sizeof(*v));
    v->id = id;
    v->type = type;
    v->parameter = parameter;
}

static void edge(struct native_function *f, struct native_block *from, unsigned long target)
{
    struct native_block *blocks = f->blocks.data, *to = &blocks[native_block(f, target)];
    struct native_point *points = to->points.data;
    size_t n, k;
    for (n = 0; n < f->variables.count; n++)
        from->out[n] |= to->in[n];
    for (n = 0; n < to->points.count; n++) {
        struct instruction *i = points[n].instruction;
        unsigned long *labels = i->labels.data;
        struct value **args = i->args.data;
        if (strcmp(i->op, "phi"))
            break;
        for (k = 0; k < i->labels.count; k++)
            if (labels[k] == from->block->id)
                break;
        if (k == i->labels.count)
            fail("phi has no argument for predecessor %lu", from->block->id);
        use(f, args[k], from->out);
    }
}

void native_analyze(struct native_function *f, struct function *function)
{
    struct parameter *parameters = function->parameters.data;
    struct block *blocks = function->blocks.data;
    struct native_block *analysis;
    unsigned char *live;
    unsigned long pc = 0;
    size_t n, k, j;
    int changed;
    f->function = function;
    if (!function->blocks.count)
        fail("native function '%s' has no body", function->name);
    if (function->blocks.count > 1024)
        fail("native feasibility function exceeds 1024 blocks");
    if (function->parameters.count + (function->vararg != 0) > 64)
        fail("native function exceeds 64 arguments");
    for (n = 0; n < function->parameters.count; n++)
        variable(f, parameters[n].id, parameters[n].type, (long)n);
    for (n = 0; n < function->blocks.count; n++) {
        struct instruction **instructions = blocks[n].instructions.data;
        struct native_block *b = append(&f->blocks, sizeof(*b));
        struct instruction *last;
        int ordinary = 0;
        for (k = 0; k < n; k++)
            if (blocks[k].id == blocks[n].id)
                fail("duplicate native block");
        b->block = &blocks[n];
        if (!blocks[n].instructions.count)
            fail("empty native block");
        for (k = 0; k < blocks[n].instructions.count; k++) {
            struct instruction *i = instructions[k];
            struct native_point *point = append(&b->points, sizeof(*point));
            point->instruction = i;
            line_number = i->line;
            if (i->result >= 0) {
                struct native_var *v;
                struct type *carrier = i->type;
                if (!strcmp(i->op, "call") &&
                    (carrier->kind == TYSTRUCT || carrier->kind == TYARRAY)) {
                    /* RCC aggregate call results carry a logical pointer to
                     * the callee's return snapshot. The frontend copies that
                     * snapshot before another activation can reuse storage.
                     * Keep the structural type on the call for ABI checking. */
                    carrier = native_scratch(sizeof(*carrier));
                    carrier->kind = TYPOINTER;
                    carrier->bits = 32;
                    carrier->size = 2;
                }
                variable(f, (unsigned long)i->result, carrier, -1);
                v = f->variables.data;
                v[f->variables.count - 1].block = n;
                v[f->variables.count - 1].position = k;
            }
            if (!strcmp(i->op, "phi")) {
                if (ordinary || i->args.count != i->labels.count)
                    fail("invalid phi placement/arity");
            } else
                ordinary = 1;
            if ((!strcmp(i->op, "ret") || !strcmp(i->op, "br")) &&
                k + 1 != blocks[n].instructions.count)
                fail("instructions after native terminator");
            if (!strcmp(i->op, "alloca")) {
                unsigned long long count = i->a ? i->a->bits : 1;
                if (i->a && i->a->kind != VNUMBER)
                    fail("native dynamic alloca is not implemented");
                if (count > 192000UL || i->source_type->size > 192000UL ||
                    count * i->source_type->size > 192000UL - f->cells)
                    fail("native logical frame exceeds stack capacity");
                i->object = f->cells;
                f->cells += (unsigned long)(count * i->source_type->size);
            }
        }
        last = instructions[blocks[n].instructions.count - 1];
        if (strcmp(last->op, "br") && strcmp(last->op, "ret"))
            fail("native block lacks terminator");
    }
    analysis = f->blocks.data;
    f->var_area = f->cells;
    {
        unsigned long extra = 0;
        for (n = 0; n < function->blocks.count; n++) {
            struct instruction **ins = blocks[n].instructions.data;
            for (k = 0; k < blocks[n].instructions.count; k++) {
                struct instruction *i = ins[k];
                unsigned long size = 0;
                struct value **args = i->args.data;
                if (strcmp(i->op, "call") || i->stack_from == 0xffffffffUL)
                    continue;
                line_number = i->line;
                if (i->stack_from > i->args.count || i->stack_from >= 64)
                    fail("invalid native variadic fixed argument count");
                for (j = i->stack_from; j < i->args.count; j++) {
                    unsigned count = native_argument_cells(f, args[j]);
                    if (count > 192000UL - size)
                        fail("native variadic area exceeds stack capacity");
                    size += count;
                }
                if (size > extra)
                    extra = size;
            }
        }
        if (extra > 192000UL - f->cells)
            fail("native logical frame exceeds stack capacity");
        f->cells += extra;
    }
    native_validate(f);
    live = native_scratch(f->variables.count);
    for (n = 0; n < f->blocks.count; n++) {
        struct native_point *points = analysis[n].points.data;
        analysis[n].in = native_scratch(f->variables.count);
        analysis[n].out = native_scratch(f->variables.count);
        for (k = 0; k < analysis[n].points.count; k++) {
            struct instruction *i = points[k].instruction;
            if (!strcmp(i->op, "call"))
                points[k].call_pc = ++pc;
            if (!f->kernel && !strcmp(i->op, "br") &&
                (native_block(f, i->yes) <= (long)n ||
                 (i->conditional && native_block(f, i->no) <= (long)n)))
                points[k].checkpoint_pc = ++pc;
            /* Only resumable instructions consume an after snapshot. Keeping
             * one at every ordinary instruction costs O(instructions * SSA
             * values), even for a straight-line function with no suspension. */
            if (points[k].call_pc || points[k].checkpoint_pc)
                points[k].after = native_scratch(f->variables.count);
            memset(live, 0, f->variables.count);
            native_before(f, i, live);
        }
    }
    do {
        changed = 0;
        for (n = f->blocks.count; n; n--) {
            struct native_block *b = &analysis[n - 1];
            struct native_point *points = b->points.data;
            struct instruction *last = points[b->points.count - 1].instruction;
            memset(b->out, 0, f->variables.count);
            if (!strcmp(last->op, "br")) {
                edge(f, b, last->yes);
                if (last->conditional)
                    edge(f, b, last->no);
            }
            memcpy(live, b->out, f->variables.count);
            for (k = b->points.count; k; k--)
                native_before(f, points[k - 1].instruction, live);
            for (j = 0; j < f->variables.count; j++)
                if (b->in[j] != live[j])
                    changed = 1;
            memcpy(b->in, live, f->variables.count);
        }
    } while (changed);
    for (n = 0; n < f->blocks.count; n++) {
        struct native_block *b = &analysis[n];
        struct native_point *points = b->points.data;
        memcpy(live, b->out, f->variables.count);
        for (k = b->points.count; k; k--) {
            if (points[k - 1].after)
                memcpy(points[k - 1].after, live, f->variables.count);
            native_before(f, points[k - 1].instruction, live);
        }
    }
}
