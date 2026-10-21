/* C16 kernel IR linkage. Native addresses never enter logical data pointers. */
#include "native.h"
#include <stdlib.h>
#include <string.h>

static void rename_value(struct module *m, struct value *v)
{
    size_t i;
    if (!v)
        return;
    if (v->kind == VGLOBAL || v->kind == VFUNCTION)
        v->name = symbol(m, v->name)->label;
    rename_value(m, v->base);
    for (i = 0; i < v->items.count; i++)
        rename_value(m, ((struct value **)v->items.data)[i]);
}

void native_kernel_merge(struct module *out, struct module *m, unsigned long unit, int shared)
{
    struct symbol *s = m->symbols.data;
    struct function *f = m->functions.data;
    size_t i, j, k, n;
    for (i = 0; i < m->symbols.count; i++) {
        if (s[i].internal) {
            char *name = allocate(strlen(s[i].name) + 48);
            sprintf(name, "__unit%lu_%s", unit, s[i].name);
            s[i].label = name;
        } else if (!strncmp(s[i].name, "__unit", 6))
            fail("reserved kernel link symbol '%s'", s[i].name);
    }
    for (i = 0; i < m->symbols.count; i++)
        rename_value(m, s[i].init);
    for (i = 0; i < m->functions.count; i++) {
        struct block *b = f[i].blocks.data;
        f[i].name = symbol(m, f[i].name)->label;
        for (j = 0; j < f[i].blocks.count; j++) {
            struct instruction **ins = b[j].instructions.data;
            for (k = 0; k < b[j].instructions.count; k++) {
                /* Intrinsic names are not linker symbols. */
                if (strcmp(ins[k]->op, "intrinsic"))
                    rename_value(m, ins[k]->a);
                rename_value(m, ins[k]->b);
                rename_value(m, ins[k]->c);
                for (n = 0; n < ins[k]->args.count; n++)
                    rename_value(m, ((struct value **)ins[k]->args.data)[n]);
            }
        }
    }
    for (i = 0; i < m->symbols.count; i++) {
        struct symbol *dest;
        s[i].name = s[i].label;
        s[i].shared = shared && s[i].defined && !s[i].function;
        dest = symbol(out, s[i].name);
        if ((dest->type || dest->defined || dest->function) && dest->function != s[i].function)
            fail("kernel function/object conflict '%s'", s[i].name);
        if (dest->defined && s[i].defined)
            fail("duplicate kernel definition '%s'", s[i].name);
        if (!s[i].function && dest->type && s[i].type) {
            int a = dest->type->kind == TYARRAY || dest->type->kind == TYSTRUCT;
            int b = s[i].type->kind == TYARRAY || s[i].type->kind == TYSTRUCT;
            if (a != b ||
                (dest->type->size && s[i].type->size && dest->type->size != s[i].type->size) ||
                (!a &&
                 (dest->type->kind != s[i].type->kind || dest->type->bits != s[i].type->bits)))
                fail("incompatible kernel global '%s'", s[i].name);
        }
        if (s[i].defined || !dest->type) {
            int defined = dest->defined;
            *dest = s[i];
            dest->defined |= defined;
        }
    }
    for (i = 0; i < m->functions.count; i++) {
        struct function *dest = out->functions.data;
        for (j = 0; j < out->functions.count; j++)
            if (!strcmp(dest[j].name, f[i].name))
                break;
        if (j == out->functions.count)
            *(struct function *)append(&out->functions, sizeof *f) = f[i];
        else {
            if (strcmp(native_signature(&dest[j]), native_signature(&f[i])))
                fail("incompatible kernel function '%s'", f[i].name);
            if (!f[i].external)
                dest[j] = f[i];
        }
    }
    /* The merged module owns the copied descriptors and their child vectors.
     * Only the redundant top-level source tables can be reclaimed here. */
    free(m->functions.data);
    free(m->symbols.data);
}

static void mark(struct module *m, unsigned char *live, struct value *v)
{
    struct symbol *s = m->symbols.data;
    size_t i;
    if (!v)
        return;
    if (v->kind == VGLOBAL || v->kind == VFUNCTION) {
        for (i = 0; i < m->symbols.count; i++)
            if (!strcmp(s[i].name, v->name)) {
                live[i] = 1;
                break;
            }
        if (i == m->symbols.count)
            fail("kernel reference has no declaration '%s'", v->name);
    }
    mark(m, live, v->base);
    for (i = 0; i < v->items.count; i++)
        mark(m, live, ((struct value **)v->items.data)[i]);
}

void native_kernel_reachable(struct module *m)
{
    struct symbol *s = m->symbols.data;
    struct function *f = m->functions.data;
    unsigned char *live = allocate(m->symbols.count), *done = allocate(m->symbols.count);
    size_t i, j, k, n, kept;
    int changed;
    if (!native_find_function(m, "main"))
        fail("kernel requires a main definition");
    for (i = 0; i < m->symbols.count; i++)
        live[i] = !strcmp(s[i].name, "main");
    do {
        changed = 0;
        for (i = 0; i < m->symbols.count; i++) {
            if (!live[i] || done[i])
                continue;
            done[i] = 1;
            changed = 1;
            mark(m, live, s[i].init);
            if (!s[i].function)
                continue;
            for (j = 0; j < m->functions.count; j++)
                if (!strcmp(f[j].name, s[i].name)) {
                    struct block *b = f[j].blocks.data;
                    if (!f[j].external)
                        native_prune(&f[j]);
                    for (k = 0; k < f[j].blocks.count; k++) {
                        struct instruction **ins = b[k].instructions.data;
                        for (n = 0; n < b[k].instructions.count; n++) {
                            size_t a;
                            if (strcmp(ins[n]->op, "intrinsic"))
                                mark(m, live, ins[n]->a);
                            mark(m, live, ins[n]->b);
                            mark(m, live, ins[n]->c);
                            for (a = 0; a < ins[n]->args.count; a++)
                                mark(m, live, ((struct value **)ins[n]->args.data)[a]);
                        }
                    }
                }
        }
    } while (changed);
    for (i = kept = 0; i < m->functions.count; i++) {
        for (j = 0; j < m->symbols.count; j++)
            if (!strcmp(f[i].name, s[j].name))
                break;
        if (j < m->symbols.count && live[j])
            f[kept++] = f[i];
    }
    m->functions.count = kept;
    for (i = kept = 0; i < m->symbols.count; i++)
        if (live[i])
            s[kept++] = s[i];
    m->symbols.count = kept;
}
