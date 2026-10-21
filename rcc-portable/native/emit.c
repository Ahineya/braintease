/* All guest-visible addresses remain logical C16 pointers. QBE owns only
 * native registers/spills; opaque runtime helpers own memory and resumptions. */
#include "native.h"
#include <stdlib.h>
#include <string.h>
#define P(...) fprintf(o->file, __VA_ARGS__)

static void function(struct native_output *o, struct function *function)
{
    struct native_function f = {0};
    struct native_var *vars;
    struct native_block *blocks;
    size_t n, k;
    native_scratch_reset();
    f.kernel = o->kernel;
    native_analyze(&f, function);
    o->function = &f;
    vars = f.variables.data;
    blocks = f.blocks.data;
    if (!o->relocatable)
        P("# rcc-function %lu %lu rcc_user_%s\n", function->serial,
          (unsigned long)function->parameters.count + (function->vararg != 0), function->name);
    P("%sfunction l $rcc_user_%s(l %%ctx) {\n@entry\n",
      !o->relocatable || !symbol(o->module, function->name)->internal ? "export " : "",
      function->name);
    P("    %%frame =l call $rcc_native_process_frame(l %%ctx, l %lu)\n    %%pc =l call "
      "$rcc_native_process_pc(l %%ctx)\n",
      f.cells);
    for (n = 0; n < f.variables.count; n++)
        P("    %%v%lu =l copy 0\n", vars[n].id);
    P("    %%initial =w ceql %%pc, 0\n    jnz %%initial, @initial, @dispatch0\n@initial\n");
    for (n = 0; n < f.variables.count; n++)
        if (vars[n].parameter >= 0) {
            char *raw = native_temporary(o);
            P("    %s =l call $rcc_native_process_arg(l %%ctx, l %ld)\n", raw, vars[n].parameter);
            P("    %%v%lu =l copy %s\n", vars[n].id, native_mask(o, raw, vars[n].type->bits));
        }
    P("    jmp @block%lu\n", blocks[0].block->id);
    for (n = 0; n < f.blocks.count; n++) {
        struct native_point *points = blocks[n].points.data;
        P("@block%lu\n", blocks[n].block->id);
        for (k = 0; k < blocks[n].points.count; k++) {
            struct instruction *i = points[k].instruction;
            line_number = i->line;
            if (!strcmp(i->op, "call"))
                native_call(o, &points[k]);
            else if (!strcmp(i->op, "br"))
                native_branch(o, &blocks[n], &points[k]);
            else if (!strcmp(i->op, "ret")) {
                unsigned bits =
                    function->result->kind == TYSTRUCT || function->result->kind == TYARRAY
                        ? 32
                        : function->result->bits;
                char *value = i->a ? native_mask(o, native_value(o, i->a), bits) : native_copy("0");
                P("    call $rcc_native_process_return(l %%ctx, l %s)\n    ret 0\n", value);
            } else if (strcmp(i->op, "phi"))
                native_instruction(o, i);
        }
    }
    {
        unsigned long previous = 0;
        for (n = 0; n < f.blocks.count; n++) {
            struct native_point *points = blocks[n].points.data;
            for (k = 0; k < blocks[n].points.count; k++) {
                unsigned long pc = points[k].call_pc ? points[k].call_pc : points[k].checkpoint_pc;
                if (pc) {
                    char *equal = native_temporary(o);
                    P("@dispatch%lu\n    %s =w ceql %%pc, %lu\n    jnz %s, @resume%lu, "
                      "@dispatch%lu\n",
                      previous, equal, pc, equal, pc, pc);
                    previous = pc;
                }
            }
        }
        P("@dispatch%lu\n    call $rcc_native_process_bad_pc(l %%ctx)\n    ret 0\n}\n\n", previous);
    }
    o->function = NULL;
    for (n = 0; n < f.blocks.count; n++)
        free(blocks[n].points.data);
    free(f.blocks.data);
    free(f.variables.data);
    native_scratch_reset();
}

static void emit(struct module *m, FILE *file, int relocatable, int no_start, int kernel)
{
    struct native_output output = {0}, *o = &output;
    struct function *functions = m->functions.data, *main = native_find_function(m, "main");
    struct symbol *symbols = m->symbols.data;
    unsigned long serial = 1;
    size_t n;
    o->module = m;
    o->file = file;
    o->relocatable = relocatable;
    o->no_start = no_start;
    o->kernel = kernel;
    for (n = 0; n < m->functions.count; n++)
        if (!functions[n].external && !kernel)
            native_prune(&functions[n]);
    if (!main && !relocatable)
        fail("native feasibility entry requires int main(void)");
    if (main) {
        struct parameter *p = main->parameters.data;
        size_t argc = main->parameters.count;
        if (main->vararg || main->result->bits != 16 || main->result->kind != TYINT ||
            (argc && (!relocatable || (argc != 2 && argc != 3))) ||
            (argc && (p[0].type->kind != TYINT || p[0].type->bits != 16)) ||
            (argc && (p[1].type->kind != TYPOINTER || p[1].type->bits != 32)) ||
            (argc == 3 && (p[2].type->kind != TYPOINTER || p[2].type->bits != 32)))
            fail("native entry requires int main(void), main(int,char**) or "
                 "main(int,char**,char**)");
    }
    if (relocatable)
        native_metadata(o);
    else
        for (n = 0; n < m->symbols.count; n++)
            if (!symbols[n].function)
                fail("native feasibility globals/initializers require --relocatable");
    for (n = 0; n < m->functions.count; n++)
        if (!functions[n].external)
            functions[n].serial = ++serial;
    if (kernel) {
        native_kernel_entry(o);
        goto functions;
    }
    if (!main || no_start)
        goto functions;
    if (!relocatable)
        P("# rcc-entry 1\n# rcc-function 1 1 rcc_user_entry\n");
    P("export function l $%s(l %%ctx) {\n@entry\n",
      relocatable ? "rcc_start_entry" : "rcc_user_entry");
    P("    %%request =l call $rcc_native_process_frame(l %%ctx, l 10)\n");
    P("    %%pc =l call $rcc_native_process_pc(l %%ctx)\n    %%initial =w ceql %%pc, 0\n    jnz "
      "%%initial, @invoke, @returned\n@invoke\n");
    if (relocatable) {
        char *local = native_binding_value(o, NM_FUNCTION, "main");
        P("    %%token =l call $rcc_native_image_token(l %%ctx, l %s)\n", local);
    } else
        P("    %%token =l call $rcc_native_image_token(l %%ctx, l %lu)\n",
          0x80000000UL | main->serial);
    if (main->parameters.count) {
        /* Existing kernel argument block: unsigned argc; char **argv, **envp.
         * The outer arity-one activation receives its logical address. */
        P("    %%startup_args =l call $rcc_native_process_arg(l %%ctx, l 0)\n"
          "    %%startup_argc =l call $rcc_native_load(l %%ctx, l %%startup_args, l 1)\n"
          "    %%startup_vector =l call $rcc_native_gep(l %%ctx, l %%startup_args, l 1)\n"
          "    %%startup_argv =l call $rcc_native_load(l %%ctx, l %%startup_vector, l 2)\n"
          "    call $rcc_native_process_argument(l %%ctx, l 0, l %%startup_argc)\n"
          "    call $rcc_native_process_argument(l %%ctx, l 1, l %%startup_argv)\n");
        if (main->parameters.count == 3)
            P("    %%startup_environment =l call $rcc_native_gep(l %%ctx, l %%startup_args, l 3)\n"
              "    %%startup_envp =l call $rcc_native_load(l %%ctx, l %%startup_environment, l 2)\n"
              "    call $rcc_native_process_argument(l %%ctx, l 2, l %%startup_envp)\n");
    }
    P("    call $rcc_native_process_set_pc(l %%ctx, l 1)\n"
      "    call $rcc_native_process_call(l %%ctx, l %%token, l %lu, l 1)\n"
      "    ret 0\n@returned\n",
      (unsigned long)main->parameters.count);
    P("    %%valid =w ceql %%pc, 1\n    jnz %%valid, @finish, @bad\n@bad\n    call "
      "$rcc_native_process_bad_pc(l %%ctx)\n    ret 0\n@finish\n");
    /* Minimal freestanding startup: notify the real kernel with the existing
     * struct request { unsigned call; long arg[4]; int error; } C16 ABI.
     * Returning the outer activation would stop the entire native scheduler.
     * Full libc init/atexit/stdio integration is a subsequent SDK milestone. */
    P("    %%status =l call $rcc_native_process_slot(l %%ctx, l 0)\n");
    P("    call $rcc_native_store(l %%ctx, l %%request, l 0, l 4)\n");
    P("    %%tail4 =l call $rcc_native_gep(l %%ctx, l %%request, l 4)\n"
      "    call $rcc_native_store(l %%ctx, l %%tail4, l 0, l 4)\n");
    P("    %%tail8 =l call $rcc_native_gep(l %%ctx, l %%request, l 8)\n"
      "    call $rcc_native_store(l %%ctx, l %%tail8, l 0, l 2)\n");
    P("    %%argument =l call $rcc_native_gep(l %%ctx, l %%request, l 1)\n"
      "    call $rcc_native_store(l %%ctx, l %%argument, l %%status, l 2)\n"
      "    call $rcc_native_process_set_pc(l %%ctx, l 2)\n"
      "    call $rcc_native_process_request(l %%ctx, l 12, l %%request, l 0, l 0)\n"
      "    ret 0\n}\n\n");
functions:
    for (n = 0; n < m->functions.count; n++)
        if (!functions[n].external)
            function(o, &functions[n]);
    if (relocatable && !kernel)
        native_metadata_write(o);
    native_scratch_reset();
}

void native_emit(struct module *m, FILE *file)
{
    emit(m, file, 0, 0, 0);
}

void native_emit_relocatable(struct module *m, FILE *file)
{
    emit(m, file, 1, 0, 0);
}

void native_emit_library(struct module *m, FILE *file)
{
    emit(m, file, 1, 1, 0);
}

void native_emit_kernel(struct module *m, FILE *file)
{
    native_kernel_reachable(m);
    emit(m, file, 1, 1, 1);
}
