/* Check dominance before QBE's mutable-name SSA construction. Initial zero
 * carriers are code-generation scaffolding, never a substitute for a real IR
 * definition on every reachable path. */
#include "native.h"
#include <string.h>
#include <stdlib.h>

struct edges {
    long yes, no;
    struct vec predecessors;
};

static void value(struct native_function *f, struct value *v, size_t block, size_t position,
                  unsigned char **dominators)
{
    struct native_var *vars = f->variables.data, *definition;
    if (!v)
        return;
    if (v->kind == VFAT || v->kind == VOFFSET) {
        value(f, v->base, block, position, dominators);
        return;
    }
    if (v->kind != VTEMP)
        return;
    definition = &vars[native_variable(f, v->id)];
    if (definition->parameter >= 0)
        return;
    if (!dominators[block][definition->block] ||
        (definition->block == block && definition->position >= position))
        fail("native IR definition of %%%lu does not dominate its use", v->id);
}

void native_validate(struct native_function *f)
{
    struct native_block *blocks = f->blocks.data;
    size_t count = f->blocks.count, n, k, j, p;
    unsigned char **dom = native_scratch(count * sizeof(*dom));
    unsigned char *next = native_scratch(count), *reachable = native_scratch(count);
    struct edges *edges = native_scratch(count * sizeof(*edges));
    int changed;
    for (n = 0; n < count; n++) {
        struct native_point *points = blocks[n].points.data;
        struct instruction *last = points[blocks[n].points.count - 1].instruction;
        edges[n].yes = edges[n].no = -1;
        if (strcmp(last->op, "br"))
            continue;
        edges[n].yes = native_block(f, last->yes);
        *(size_t *)append(&edges[edges[n].yes].predecessors, sizeof(size_t)) = n;
        if (last->conditional) {
            edges[n].no = native_block(f, last->no);
            if (edges[n].yes != edges[n].no)
                *(size_t *)append(&edges[edges[n].no].predecessors, sizeof(size_t)) = n;
        }
    }
    reachable[0] = 1;
    do {
        changed = 0;
        for (n = 0; n < count; n++)
            if (reachable[n]) {
                long targets[2];
                targets[0] = edges[n].yes;
                targets[1] = edges[n].no;
                for (k = 0; k < 2; k++)
                    if (targets[k] >= 0 && !reachable[targets[k]]) {
                        reachable[targets[k]] = 1;
                        changed = 1;
                    }
            }
    } while (changed);
    for (n = 0; n < count; n++) {
        if (!reachable[n])
            fail("native feasibility backend requires reachable blocks");
        dom[n] = native_scratch(count);
        if (n)
            memset(dom[n], 1, count);
        else
            dom[n][0] = 1;
    }
    do {
        changed = 0;
        for (n = 1; n < count; n++) {
            size_t *predecessors = edges[n].predecessors.data;
            memset(next, 1, count);
            for (p = 0; p < edges[n].predecessors.count; p++)
                for (k = 0; k < count; k++)
                    next[k] &= dom[predecessors[p]][k];
            next[n] = 1;
            if (memcmp(dom[n], next, count)) {
                memcpy(dom[n], next, count);
                changed = 1;
            }
        }
    } while (changed);
    for (n = 0; n < count; n++) {
        struct native_point *points = blocks[n].points.data;
        for (k = 0; k < blocks[n].points.count; k++) {
            struct instruction *i = points[k].instruction;
            struct value **args = i->args.data;
            line_number = i->line;
            if (!strcmp(i->op, "phi")) {
                unsigned long *labels = i->labels.data;
                if (i->labels.count != edges[n].predecessors.count)
                    fail("phi predecessor count mismatch");
                for (j = 0; j < i->labels.count; j++) {
                    size_t source = (size_t)native_block(f, labels[j]);
                    size_t q;
                    for (q = 0; q < j; q++)
                        if (labels[q] == labels[j])
                            fail("duplicate phi predecessor");
                    if (edges[source].yes != (long)n && edges[source].no != (long)n)
                        fail("phi names a non-predecessor block");
                    value(f, args[j], source, (size_t)-1, dom);
                }
            } else {
                value(f, i->a, n, k, dom);
                value(f, i->b, n, k, dom);
                value(f, i->c, n, k, dom);
                for (j = 0; j < i->args.count; j++)
                    value(f, args[j], n, k, dom);
            }
        }
    }
    for (n = 0; n < count; n++)
        free(edges[n].predecessors.data);
}
