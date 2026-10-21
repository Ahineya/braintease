/* Remove dead CFG blocks before metadata and frame/liveness analysis. Validate
 * the original graph first so malformed edges cannot disappear with dead code. */
#include "native.h"
#include <string.h>
#include <stdlib.h>

static size_t index_of(struct function *f, unsigned long id)
{
    struct block *blocks = f->blocks.data;
    size_t n;
    for (n = 0; n < f->blocks.count; n++)
        if (blocks[n].id == id)
            return n;
    fail("undefined native IR block %lu", id);
    return 0;
}

void native_prune(struct function *f)
{
    struct block *blocks = f->blocks.data;
    size_t count = f->blocks.count, n, k, j, kept;
    unsigned char *live;
    unsigned char **edges;
    int changed;
    if (!count || count > 1024)
        fail("native function requires 1..1024 blocks");
    live = calloc(count, 1);
    edges = calloc(count, sizeof(*edges));
    if (!live || !edges)
        fail("out of memory checking native control flow");
    for (n = 0; n < count; n++) {
        struct instruction **ins = blocks[n].instructions.data, *last;
        int ordinary = 0;
        edges[n] = calloc(count, 1);
        if (!edges[n])
            fail("out of memory checking native control flow");
        for (k = 0; k < n; k++)
            if (blocks[n].id == blocks[k].id)
                fail("duplicate native block");
        if (!blocks[n].instructions.count)
            fail("empty native block");
        for (k = 0; k < blocks[n].instructions.count; k++) {
            line_number = ins[k]->line;
            if (!strcmp(ins[k]->op, "phi")) {
                if (!n || ordinary || !ins[k]->args.count ||
                    ins[k]->args.count != ins[k]->labels.count)
                    fail("invalid phi placement/arity");
            } else
                ordinary = 1;
            if ((!strcmp(ins[k]->op, "ret") || !strcmp(ins[k]->op, "br")) &&
                k + 1 != blocks[n].instructions.count)
                fail("instructions after native terminator");
        }
        last = ins[blocks[n].instructions.count - 1];
        if (!strcmp(last->op, "br")) {
            edges[n][index_of(f, last->yes)] = 1;
            if (last->conditional)
                edges[n][index_of(f, last->no)] = 1;
        } else if (strcmp(last->op, "ret"))
            fail("native block lacks terminator");
    }
    for (n = 0; n < count; n++) {
        struct instruction **ins = blocks[n].instructions.data;
        size_t predecessors = 0;
        for (k = 0; k < count; k++)
            predecessors += edges[k][n];
        for (k = 0; k < blocks[n].instructions.count && !strcmp(ins[k]->op, "phi"); k++) {
            unsigned long *labels = ins[k]->labels.data;
            line_number = ins[k]->line;
            if (ins[k]->labels.count != predecessors)
                fail("phi predecessor count mismatch");
            for (j = 0; j < ins[k]->labels.count; j++) {
                size_t q;
                if (!edges[index_of(f, labels[j])][n])
                    fail("phi names a non-predecessor block");
                for (q = 0; q < j; q++)
                    if (labels[q] == labels[j])
                        fail("duplicate phi predecessor");
            }
        }
    }
    live[0] = 1;
    do {
        changed = 0;
        for (n = 0; n < count; n++)
            if (live[n])
                for (k = 0; k < count; k++)
                    if (edges[n][k] && !live[k]) {
                        live[k] = 1;
                        changed = 1;
                    }
    } while (changed);
    for (n = 0; n < count; n++)
        if (live[n]) {
            struct instruction **ins = blocks[n].instructions.data;
            for (k = 0; k < blocks[n].instructions.count && !strcmp(ins[k]->op, "phi"); k++) {
                unsigned long *labels = ins[k]->labels.data;
                struct value **args = ins[k]->args.data;
                for (j = kept = 0; j < ins[k]->labels.count; j++)
                    if (live[index_of(f, labels[j])]) {
                        labels[kept] = labels[j];
                        args[kept++] = args[j];
                    }
                ins[k]->labels.count = ins[k]->args.count = kept;
            }
        }
    for (n = kept = 0; n < count; n++)
        if (live[n])
            blocks[kept++] = blocks[n];
    f->blocks.count = kept;
    for (n = 0; n < count; n++)
        free(edges[n]);
    free(edges);
    free(live);
}
