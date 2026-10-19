/* Streaming linker: only symbols/dependencies live in memory. Code and data
 * are read from object files as the executable is written. Chunk-relative
 * object addresses need only a base addition at link time. */
#include <stdlib.h>
#include <string.h>
#include "tools.h"
static struct array objects;
static unsigned long total_code, total_data, dispatcher;
static struct array initializers;
static bool have_dispatcher;

struct target {
    struct object *object;
    unsigned kind;
    unsigned long value;
};

static bool find(const char *name, struct target *target, bool selected)
{
    size_t i, j;
    struct object **all = objects.data;
    for (i = 0; i < objects.count; i++)
        if (!selected || all[i]->selected) {
            struct definition *d = all[i]->defs.data;
            for (j = 0; j < all[i]->defs.count; j++)
                if (!strcmp(d[j].name, name)) {
                    target->object = all[i];
                    target->kind = d[j].kind;
                    target->value = d[j].value;
                    return true;
                }
        }
    return false;
}

static bool builtin(const char *name)
{
    return !strcmp(name, "_init_globals") || !strcmp(name, "__rcc_layout_stack_bank") ||
           !strcmp(name, "__rcc_layout_heap_bank");
}

static void addfile(const char *name)
{
    FILE *f = open_input(name);
    char marker[5];
    unsigned long count, i;
    struct object *o;
    if (fread(marker, 1, 4, f) != 4)
        fatal("truncated input '%s'", name);
    marker[4] = 0;
    if (!strcmp(marker, "RPA1")) {
        count = get32(f);
        if (count > 10000)
            fatal("archive contains too many objects");
        for (i = 0; i < count; i++) {
            unsigned long size = get32(f), start = (unsigned long)ftell(f);
            o = read_object(f, name);
            if ((unsigned long)ftell(f) - start != size)
                fatal("archive member size mismatch");
            *(struct object **)array_append(&objects, sizeof(o)) = o;
        }
    } else if (!strcmp(marker, "RPO1")) {
        rewind(f);
        o = read_object(f, name);
        o->selected = true;
        *(struct object **)array_append(&objects, sizeof(o)) = o;
    } else
        fatal("'%s' is not an RPO1 object or RPA1 archive", name);
    fclose(f);
}

static void select_objects(void)
{
    bool changed = true;
    size_t i, j, k, l;
    struct target t;
    struct object **all = objects.data;
    while (changed) {
        changed = false;
        for (i = 0; i < objects.count; i++)
            if (all[i]->selected) {
                char **ext = all[i]->externs.data;
                for (j = 0; j < all[i]->externs.count; j++) {
                    if (builtin(ext[j]) || find(ext[j], &t, true))
                        continue;
                    if (!find(ext[j], &t, false))
                        fatal("undefined symbol '%s'", ext[j]);
                    t.object->selected = true;
                    changed = true;
                }
            }
    }
    for (i = 0; i < objects.count; i++)
        if (all[i]->selected) {
            struct definition *a = all[i]->defs.data;
            for (j = 0; j < all[i]->defs.count; j++) {
                for (k = 0; k <= i; k++)
                    if (all[k]->selected) {
                        struct definition *b = all[k]->defs.data;
                        for (l = 0; l < (k == i ? j : all[k]->defs.count); l++)
                            if (!strcmp(a[j].name, b[l].name))
                                fatal("duplicate symbol '%s'", a[j].name);
                    }
            }
        }
}

static void layout(void)
{
    size_t i, j;
    unsigned banked;
    struct object **all = objects.data;
    struct target t;
    for (i = 0; i < objects.count; i++)
        if (all[i]->selected) {
            if (all[i]->ncode > 16000000UL - total_code)
                fatal("linked code is too large");
            all[i]->base = total_code;
            total_code += all[i]->ncode;
        }
    for (banked = 0; banked < 2; banked++)
        for (i = 0; i < objects.count; i++)
            if (all[i]->selected && all[i]->banked == (banked != 0)) {
                all[i]->database = total_data;
                total_data += all[i]->ndata;
                if (total_data >= 16UL * BANK)
                    fatal("linked globals exceed the RXE data limit");
                if (!banked && total_data > BANK)
                    fatal("legacy globals exceed one bank");
            }
    have_dispatcher = !find("_init_globals", &t, true);
    if (have_dispatcher) {
        for (i = 0; i < objects.count; i++)
            if (all[i]->selected) {
                struct definition *d = all[i]->defs.data;
                for (j = 0; j < all[i]->defs.count; j++)
                    if (!d[j].kind && !strncmp(d[j].name, "__rcc_init_", 11)) {
                        struct target *p = array_append(&initializers, sizeof(*p));
                        p->object = all[i];
                        p->kind = 0;
                        p->value = d[j].value;
                    }
            }
        /* The dispatcher is one small, independently chunk-packed object. */
        dispatcher = total_code;
        total_code += ((initializers.count + 9 + PAYLOAD - 1) / PAYLOAD) * CHUNK;
    }
    if (!total_code)
        fatal("no executable code");
}

static void resolve(struct object *o, struct record *r)
{
    struct target t;
    unsigned long location;
    long addend = r->addend;
    const char *name = NULL;
    if (r->kind == RNONE)
        return;
    t.object = o;
    t.kind = r->target == TLOCALDATA ? 1 : 0;
    t.value = r->value;
    if (r->target == TEXTERNAL) {
        if (r->value >= o->externs.count)
            fatal("relocation external index out of range");
        name = ((char **)o->externs.data)[r->value];
        if (!strcmp(name, "_init_globals") && have_dispatcher) {
            t.object = NULL;
            t.kind = 0;
            t.value = dispatcher;
        } else if (!strcmp(name, "__rcc_layout_stack_bank") ||
                   !strcmp(name, "__rcc_layout_heap_bank")) {
            unsigned long stack = 1 + (total_data + BANK - 1) / BANK;
            if (stack < 2)
                stack = 2;
            if (r->kind != RDATA || r->op != OP_LI)
                fatal("layout constant requires LI");
            r->b = (unsigned)(!strcmp(name, "__rcc_layout_stack_bank") ? stack
                              : stack + 1 < 5                          ? 5
                                                                       : stack + 1);
            r->kind = RNONE;
            return;
        } else if (!find(name, &t, true))
            fatal("undefined symbol '%s'", name);
    } else if (r->target != TLOCALDATA && r->target != TLOCALCODE)
        fatal("invalid relocation target");
    if (t.object) {
        if (t.value >= (t.kind ? t.object->ndata : t.object->ncode))
            fatal("relocation target outside object");
        location = (t.kind ? t.object->database : t.object->base) + t.value;
    } else
        location = t.value;
    if (addend < 0) {
        unsigned long n = 0UL - (unsigned long)addend;
        if (n > location)
            fatal("negative data projection");
        location -= n;
    } else {
        if ((unsigned long)addend > 0xffffffffUL - location)
            fatal("relocation overflow");
        location += (unsigned long)addend;
    }
    if (r->kind == RJAL || r->kind == RCBANK || r->kind == RCOFF) {
        if (t.kind || location >= total_code)
            fatal("code relocation names data or invalid code");
        if (r->kind == RJAL) {
            if (r->op != OP_JAL)
                fatal("jump relocation on non-JAL");
            r->b = (unsigned)(location / BANK);
            r->c = (unsigned)(location % BANK);
        } else {
            if (r->op != OP_LI)
                fatal("code projection requires LI");
            r->b = (unsigned)(r->kind == RCBANK ? location / BANK : location % BANK);
        }
    } else {
        if (!t.kind || location >= BANK * 65536UL)
            fatal("data relocation names code or invalid data");
        if (r->op != OP_LI)
            fatal("data projection requires LI");
        if (r->kind == RDBANK)
            r->b = (unsigned)(location / BANK);
        else if (r->kind == RDOFF)
            r->b = (unsigned)(location % BANK);
        else {
            if (location >= BANK)
                fatal("legacy data address requires a bank projection");
            r->b = (unsigned)location;
        }
    }
}

static void instruction(FILE *f, struct record *r)
{
    putc(r->op & 255, f);
    putc(r->op & 255, f);
    put16(f, r->a);
    put16(f, r->b);
    put16(f, r->c);
}

struct dispatcher_writer {
    FILE *out;
    unsigned long pc;
};

static void dispatch_emit(struct dispatcher_writer *w, unsigned op, unsigned a, unsigned b,
                          unsigned c)
{
    struct record r = {0};
    r.op = op;
    r.a = a;
    r.b = b;
    r.c = c;
    if ((w->pc & CHUNK_MASK) == PAYLOAD) {
        struct record bridge = {0};
        unsigned long dest = dispatcher + w->pc + 2;
        bridge.op = OP_JAL;
        bridge.b = (unsigned)(dest / BANK);
        bridge.c = (unsigned)(dest % BANK);
        instruction(w->out, &bridge);
        bridge.op = OP_NOP;
        bridge.b = 0;
        bridge.c = 0;
        instruction(w->out, &bridge);
        w->pc += 2;
    }
    instruction(w->out, &r);
    w->pc++;
}

static void write_dispatcher(FILE *out)
{
    size_t i;
    struct target *inits = initializers.data;
    struct dispatcher_writer w;
    w.out = out;
    w.pc = 0;
    dispatch_emit(&w, OP_STORE, 3, 28, 29);
    dispatch_emit(&w, OP_ADDI, 29, 29, 1);
    dispatch_emit(&w, OP_STORE, 4, 28, 29);
    dispatch_emit(&w, OP_ADDI, 29, 29, 1);
    for (i = 0; i < initializers.count; i++) {
        unsigned long dest = inits[i].object->base + inits[i].value;
        dispatch_emit(&w, OP_JAL, 3, (unsigned)(dest / BANK), (unsigned)(dest % BANK));
    }
    dispatch_emit(&w, OP_ADDI, 29, 29, 65535);
    dispatch_emit(&w, OP_LOAD, 4, 28, 29);
    dispatch_emit(&w, OP_ADDI, 29, 29, 65535);
    dispatch_emit(&w, OP_LOAD, 3, 28, 29);
    dispatch_emit(&w, OP_JALR, 0, 4, 3);
    while (dispatcher + w.pc < total_code) {
        struct record r = {0};
        r.op = OP_NOP;
        instruction(out, &r);
        w.pc++;
    }
}

static void write_reloc(FILE *f, unsigned kind, unsigned long index)
{
    put16(f, kind);
    put16(f, 0);
    put32(f, index);
}

static void write_archive(const char *dest)
{
    size_t i;
    FILE *out;
    struct object **all = objects.data;
    out = open_output(dest);
    fputs("RPA1", out);
    put32(out, (unsigned long)objects.count);
    for (i = 0; i < objects.count; i++) {
        FILE *f = open_input(all[i]->path);
        unsigned long size = all[i]->data_at + all[i]->ndata - all[i]->body, n;
        put32(out, size);
        if (fseek(f, (long)all[i]->body, SEEK_SET))
            fatal("cannot seek archive member");
        for (n = 0; n < size; n++) {
            int ch = getc(f);
            if (ch == EOF)
                fatal("truncated archive member");
            putc(ch, out);
        }
        fclose(f);
    }
    finish_output(out);
}

/* Resolve every selected record before opening the destination. */
static unsigned long validate_relocations(void)
{
    size_t i;
    unsigned long j, nreloc = 0;
    struct record r;
    struct object **all = objects.data;
    /* Validate all selected records and count runtime relocations before output. */
    for (i = 0; i < objects.count; i++)
        if (all[i]->selected) {
            FILE *f = open_input(all[i]->path);
            source_file = all[i]->path;
            if (fseek(f, (long)all[i]->code_at, SEEK_SET))
                fatal("cannot seek object code");
            for (j = 0; j < all[i]->ncode; j++) {
                get_record(f, &r);
                resolve(all[i], &r);
                if (r.kind == RJAL || r.kind == RCBANK || r.kind == RDATA)
                    nreloc++;
            }
            fclose(f);
        }
    if (have_dispatcher) {
        unsigned long size = initializers.count + 9;
        unsigned long bridges = (size - 1) / PAYLOAD;
        nreloc += initializers.count + bridges;
    }
    return nreloc;
}

static void write_code(FILE *out)
{
    size_t i;
    unsigned long j;
    struct record r;
    struct object **all = objects.data;
    for (i = 0; i < objects.count; i++)
        if (all[i]->selected) {
            FILE *f = open_input(all[i]->path);
            if (fseek(f, (long)all[i]->code_at, SEEK_SET))
                fatal("cannot seek code");
            for (j = 0; j < all[i]->ncode; j++) {
                get_record(f, &r);
                resolve(all[i], &r);
                instruction(out, &r);
            }
            fclose(f);
        }
    if (have_dispatcher)
        write_dispatcher(out);
}

static void write_data(FILE *out)
{
    size_t i;
    unsigned long j;
    struct object **all = objects.data;
    for (j = 0; j < 2; j++)
        for (i = 0; i < objects.count; i++)
            if (all[i]->selected && all[i]->banked == (j != 0)) {
                FILE *f = open_input(all[i]->path);
                unsigned long n;
                if (fseek(f, (long)all[i]->data_at, SEEK_SET))
                    fatal("cannot seek data");
                for (n = 0; n < all[i]->ndata; n++) {
                    int ch = getc(f);
                    if (ch == EOF)
                        fatal("truncated data");
                    putc(ch, out);
                }
                fclose(f);
            }
}

static void write_runtime_relocations(FILE *out)
{
    size_t i;
    unsigned long j;
    struct record r;
    struct object **all = objects.data;
    for (i = 0; i < objects.count; i++)
        if (all[i]->selected) {
            FILE *f = open_input(all[i]->path);
            if (fseek(f, (long)all[i]->code_at, SEEK_SET))
                fatal("cannot seek relocations");
            for (j = 0; j < all[i]->ncode; j++) {
                get_record(f, &r);
                resolve(all[i], &r);
                if (r.kind == RJAL || r.kind == RCBANK)
                    write_reloc(out, 0, all[i]->base + j);
                else if (r.kind == RDATA)
                    write_reloc(out, 1, all[i]->base + j);
            }
            fclose(f);
        }
    if (have_dispatcher) {
        unsigned long pc = 0, n;
        /* Same packing as dispatcher writer; only CALL and bridge relocate. */
        for (n = 0; n < initializers.count + 9; n++) {
            if ((pc & CHUNK_MASK) == PAYLOAD) {
                write_reloc(out, 0, dispatcher + pc);
                pc += 2;
            }
            if (n >= 4 && n < initializers.count + 4)
                write_reloc(out, 0, dispatcher + pc);
            pc++;
        }
    }
}

int linker(int argc, char **argv)
{
    const char *dest = NULL, *format = "rxe", *entry = "_start";
    int arg;
    size_t i;
    unsigned long nreloc, entry_at;
    struct target t;
    FILE *out;
    struct object **all;
    bool archive = false;
    for (arg = 1; arg < argc; arg++) {
        if (!strcmp(argv[arg], "-o") && arg + 1 < argc)
            dest = argv[++arg];
        else if (!strcmp(argv[arg], "-f") && arg + 1 < argc)
            format = argv[++arg];
        else if (!strcmp(argv[arg], "-e") && arg + 1 < argc)
            entry = argv[++arg];
        else if (!strcmp(argv[arg], "--archive"))
            archive = true;
        else if (argv[arg][0] == '-')
            fatal("usage: rcc-ld [-f rxe|binary] [-e ENTRY] -o EXECUTABLE OBJECTS...");
        else {
            source_file = argv[arg];
            addfile(argv[arg]);
        }
    }
    if (!dest || !objects.count || (strcmp(format, "rxe") && strcmp(format, "binary")))
        fatal("output, objects and rxe/binary format are required");
    all = objects.data;
    for (i = 0; i < objects.count; i++)
        if (!strcmp(dest, all[i]->path))
            fatal("output must not overwrite an input");
    if (archive) {
        write_archive(dest);
        return 0;
    }
    select_objects();
    layout();
    if (!find(entry, &t, true) || t.kind)
        fatal("undefined code entry '%s'", entry);
    entry_at = t.object->base + t.value;
    nreloc = validate_relocations();
    source_file = dest;
    out = open_output(dest);
    if (!strcmp(format, "rxe")) {
        fputs("RXE1", out);
        put16(out, (unsigned)BANK);
        put16(out, 0);
        put32(out, entry_at);
        put32(out, total_code);
        put32(out, total_data);
        put32(out, nreloc);
    } else {
        fputs("RLINK", out);
        put16(out, (unsigned)BANK);
        put32(out, entry_at * 4);
        put32(out, total_code);
    }
    write_code(out);
    if (!strcmp(format, "binary"))
        put32(out, total_data);
    write_data(out);
    if (!strcmp(format, "rxe"))
        write_runtime_relocations(out);
    finish_output(out);
    return 0;
}
