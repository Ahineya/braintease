/* Baseline Ripple code generation. SSA values have fixed, typed stack homes.
 * This deliberately separates correctness/ABI work from later register allocation.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "ripple.h"
static FILE *out;
static struct module *mod;
static struct function *fn;
static bool helpers[24][5];
static unsigned long label_id;
/* The ABI returns wide values in RV0, RV1, X0, X1, in low-word-first order.
 * These registers are deliberately not a contiguous register range. */
static const unsigned returns[4] = {5, 6, 11, 12};

static void emit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
}

static const char *label(const char *name)
{
    struct symbol *s = symbol(mod, name);
    if (!s->label)
        fail("symbol '%s' was not declared", name);
    return s->label;
}

static void move(unsigned to, unsigned from)
{
    if (to != from)
        emit("    ADD R%u, R%u, R0\n", to, from);
}

static void immediate(unsigned reg, unsigned long long n)
{
    emit("    LI R%u, %u\n", reg, (unsigned int)(n & 65535));
}

static void loadslot(unsigned reg, long at)
{
    emit("    ADDI SC, FP, %ld\n    LOAD R%u, SB, SC\n", at, reg);
}

static void storeslot(unsigned reg, unsigned long at)
{
    emit("    ADDI SC, FP, %lu\n    STORE R%u, SB, SC\n", at, reg);
}

static struct home *home(unsigned long id)
{
    struct home *h = fn->homes.data;
    if (id >= fn->homes.count || !h[id].type)
        fail("undefined SSA value %%%lu", id);
    return &h[id];
}

static unsigned width(struct value *v)
{
    if (!v)
        return 0;
    if (v->kind == VTEMP)
        return slots(home(v->id)->type);
    if (v->kind == VFAT || v->kind == VGLOBAL || v->kind == VFUNCTION || v->kind == VOFFSET)
        return 2;
    return 1;
}

static void save(long id, unsigned base, unsigned n)
{
    unsigned i;
    struct home *h;
    if (id < 0)
        return;
    h = home((unsigned long)id);
    if (n != slots(h->type))
        fail("internal result width mismatch");
    for (i = 0; i < n; i++)
        storeslot(base + i, h->offset + i);
}

static void helper(const char *op, unsigned n)
{
    int i;
    for (i = 0; i < 23; i++)
        if (!strcmp(op, binary_ops[i]))
            break;
    if (i == 23 || (n != 1 && n != 2 && n != 4))
        fail("invalid arithmetic helper %s/%u", op, n);
    helpers[i][n] = true;
    emit("    CALL %s_%s%u\n", mod->prefix, op, n);
}

static void concrete(unsigned bank)
{
    unsigned long id = ++label_id;
    emit("    LI SC, 65535\n    BNE R%u, SC, %s_p%lu_stack\n", bank, mod->prefix, id);
    move(bank, 31);
    emit("%s_p%lu_stack:\n    LI SC, 65534\n    BNE R%u, SC, %s_p%lu_done\n", mod->prefix, id, bank,
         mod->prefix, id);
    move(bank, 28);
    emit("%s_p%lu_done:\n", mod->prefix, id);
}

static void nextcell(unsigned addr, unsigned bank)
{
    unsigned long id = ++label_id;
    emit("    ADDI R%u, R%u, 1\n    LI SC, 64000\n    BNE R%u, SC, %s_next%lu\n    LI R%u, 0\n    "
         "ADDI R%u, R%u, 1\n%s_next%lu:\n",
         addr, addr, addr, mod->prefix, id, addr, bank, bank, mod->prefix, id);
}

static void get(struct value *, unsigned, unsigned);

static void address(struct value *v, unsigned base)
{
    get(v, base, 2);
    concrete(base + 1);
}

static void get(struct value *v, unsigned base, unsigned n)
{
    unsigned i, actual;
    if (!v)
        fail("missing IR operand");
    if (v->kind == VTEMP) {
        struct home *h = home(v->id);
        actual = slots(h->type);
        for (i = 0; i < n; i++) {
            if (i < actual)
                loadslot(base + i, (long)(h->offset + i));
            else
                immediate(base + i, 0);
        }
    } else if (v->kind == VNUMBER || v->kind == VUNDEF) {
        for (i = 0; i < n; i++)
            immediate(base + i, v->bits >> (i * 16));
    } else if (v->kind == VGLOBAL || v->kind == VFUNCTION) {
        const char *s = label(v->name);
        if (v->kind == VFUNCTION)
            emit("    LI R%u, %%offset(%s)\n", base, s);
        else
            emit("    LI R%u, %%data_offset(%s+0)\n", base, s);
        if (n > 1) {
            if (v->kind == VFUNCTION)
                emit("    LI R%u, %%bank(%s)\n", base + 1, s);
            else
                emit("    LI R%u, %%data_bank(%s+0)\n    ADD R%u, R%u, GP\n", base + 1, s, base + 1,
                     base + 1);
        }
        for (i = 2; i < n; i++)
            immediate(base + i, 0);
    } else if (v->kind == VOFFSET) {
        if (v->base->kind != VGLOBAL)
            fail("static offset requires a global base");
        emit("    LI R%u, %%data_offset(%s%+ld)\n", base, label(v->base->name), v->offset);
        if (n > 1)
            emit("    LI R%u, %%data_bank(%s%+ld)\n    ADD R%u, R%u, GP\n", base + 1,
                 label(v->base->name), v->offset, base + 1, base + 1);
        for (i = 2; i < n; i++)
            immediate(base + i, 0);
    } else if (v->kind == VFAT) {
        get(v->base, base, n);
        if (n > 1 && v->bank != BMIXED && v->base->kind != VGLOBAL && v->base->kind != VOFFSET &&
            v->base->kind != VFUNCTION) {
            if (v->bank == BGLOBAL)
                move(base + 1, 31);
            else if (v->bank == BSTACK)
                move(base + 1, 28);
            else
                immediate(base + 1, v->bank == BNULL ? 65533 : (unsigned int)v->bank);
        }
    } else
        fail("aggregate value cannot be used as a scalar");
}

/* The SDK stores null as (0, -3), and may return GP/SB provenance tags.
 * Integer conversions and address arithmetic need concrete components. */
static void components(struct value *v, unsigned base)
{
    unsigned long id = ++label_id;
    get(v, base, 2);
    concrete(base + 1);
    emit("    LI SC, 65533\n    BNE R%u, SC, %s_null%lu_done\n", base + 1, mod->prefix, id);
    immediate(base, 0);
    immediate(base + 1, 0);
    emit("%s_null%lu_done:\n", mod->prefix, id);
}

static void memory(bool store, struct value *pointer, unsigned base, unsigned n)
{
    unsigned i;
    address(pointer, 19);
    for (i = 0; i < n; i++) {
        emit("    %s R%u, T5, T4\n", store ? "STORE" : "LOAD", base + i);
        if (i + 1 < n)
            nextcell(19, 20);
    }
}

static void definehome(unsigned long id, struct type *type)
{
    struct home *h;
    while (fn->homes.count <= id)
        append(&fn->homes, sizeof(*h));
    h = fn->homes.data;
    if (h[id].defined)
        fail("SSA value %%%lu is defined twice", id);
    h[id].type = type;
    h[id].defined = true;
}

static struct block *block(unsigned long id)
{
    size_t i;
    struct block *b = fn->blocks.data;
    for (i = 0; i < fn->blocks.count; i++)
        if (b[i].id == id)
            return &b[i];
    fail("unknown branch target %lu", id);
    return NULL;
}

static void layout(struct function *f)
{
    size_t i, j;
    unsigned long offset = 0;
    struct parameter *p = f->parameters.data;
    struct block *b = f->blocks.data;
    struct home *h;
    fn = f;
    for (i = 0; i < f->parameters.count; i++)
        definehome(p[i].id, p[i].type);
    for (i = 0; i < f->blocks.count; i++) {
        if (!b[i].instructions.count)
            fail("empty block %lu", b[i].id);
        for (j = 0; j < i; j++)
            if (b[i].id == b[j].id)
                fail("duplicate block %lu", b[i].id);
        for (j = 0; j < b[i].instructions.count; j++) {
            struct instruction *in = ((struct instruction **)b[i].instructions.data)[j];
            line_number = in->line;
            if (in->result >= 0) {
                if (!in->type || in->type->kind == TYVOID)
                    fail("result requires a non-void type");
                definehome((unsigned long)in->result, in->type);
            }
        }
    }
    h = f->homes.data;
    for (i = 0; i < f->homes.count; i++)
        if (h[i].type) {
            h[i].offset = offset;
            offset += slots(h[i].type);
            if (offset > 32000)
                fail("function '%s' exceeds the baseline 32000-cell frame limit", f->name);
        }
    for (i = 0; i < f->blocks.count; i++)
        for (j = 0; j < b[i].instructions.count; j++) {
            struct instruction *in = ((struct instruction **)b[i].instructions.data)[j];
            line_number = in->line;
            if (!strcmp(in->op, "alloca")) {
                unsigned long long count = in->a ? in->a->bits : 1;
                if (in->a && in->a->kind != VNUMBER)
                    fail("dynamic alloca is not implemented");
                if (!in->source_type->size || count > 32000 / in->source_type->size)
                    fail("alloca exceeds baseline frame limit");
                in->object = offset;
                offset += (unsigned long)count * in->source_type->size;
            } else if (!strcmp(in->op, "phi")) {
                in->shadow = offset;
                offset += slots(in->type);
            }
            if (offset > 32000)
                fail("function '%s' exceeds the baseline 32000-cell frame limit", f->name);
            if (!strcmp(in->op, "br")) {
                block(in->yes);
                if (in->conditional)
                    block(in->no);
            }
            if (j + 1 == b[i].instructions.count) {
                if (strcmp(in->op, "br") && strcmp(in->op, "ret"))
                    fail("block %lu lacks a terminator", b[i].id);
            } else if (!strcmp(in->op, "br") || !strcmp(in->op, "ret"))
                fail("instruction follows terminator");
        }
    f->frame = offset;
}

static void edge(unsigned long from, unsigned long to)
{
    size_t i, j;
    struct block *b = block(to);
    struct instruction **in = b->instructions.data;
    /* Stage all incoming values before writing any destination (parallel copy). */
    for (i = 0; i < b->instructions.count; i++)
        if (!strcmp(in[i]->op, "phi")) {
            struct value **v = in[i]->args.data;
            unsigned long *labels = in[i]->labels.data;
            for (j = 0; j < in[i]->labels.count; j++)
                if (labels[j] == from)
                    break;
            if (j == in[i]->labels.count)
                fail("phi lacks incoming edge from block %lu", from);
            get(v[j], 15, slots(in[i]->type));
            {
                unsigned w;
                for (w = 0; w < slots(in[i]->type); w++)
                    storeslot(15 + w, in[i]->shadow + w);
            }
        }
    for (i = 0; i < b->instructions.count; i++)
        if (!strcmp(in[i]->op, "phi")) {
            unsigned w;
            for (w = 0; w < slots(in[i]->type); w++)
                loadslot(15 + w, (long)(in[i]->shadow + w));
            save(in[i]->result, 15, slots(in[i]->type));
        }
    emit("    JAL R0, 0, %s_f%lu_b%lu\n", mod->prefix, fn->serial, to);
}

static void prologue(unsigned long extra)
{
    /* FP follows saved RA, RAB, FP and S0..S3. Incoming stack arguments
     * therefore start below FP-7; local homes occupy FP and higher cells. */
    unsigned long id = ++label_id;
    unsigned i;
    const unsigned saved[7] = {3, 4, 30, 23, 24, 25, 26};
    if (extra > 63990)
        fail("frame exceeds one stack bank");
    emit("    LI X0, %lu\n    ADD X0, SP, X0\n    SLTU X1, X0, SP\n    BNE X1, R0, "
         "%s_stack%lu_bad\n    LI X1, 64000\n    SLTU X1, X0, X1\n    BNE X1, R0, "
         "%s_stack%lu_ok\n%s_stack%lu_bad:\n    BRK\n%s_stack%lu_ok:\n",
         extra + 7, mod->prefix, id, mod->prefix, id, mod->prefix, id, mod->prefix, id);
    for (i = 0; i < 7; i++)
        emit("    STORE R%u, SB, SP\n    ADDI SP, SP, 1\n", saved[i]);
    move(30, 29);
    emit("    ADDI SP, SP, %lu\n", fn->frame);
}

static void epilogue(void)
{
    int i;
    const unsigned saved[7] = {3, 4, 30, 23, 24, 25, 26};
    emit("    ADDI SP, FP, -7\n");
    for (i = 6; i >= 0; i--) {
        emit("    ADDI SC, SP, %d\n    LOAD R%u, SB, SC\n", i, saved[i]);
    }
    emit("    RET\n");
}

static void parameters(void)
{
    struct parameter *p = fn->parameters.data;
    size_t i;
    unsigned used = 0, w;
    bool stack = false;
    long offset = -8;
    for (i = 0; i < fn->parameters.count; i++) {
        unsigned n = slots(p[i].type);
        if (used + n > 4)
            stack = true;
        for (w = 0; w < n; w++) {
            if (stack) {
                loadslot(15, offset - (long)w);
                storeslot(15, home(p[i].id)->offset + w);
            } else
                storeslot(7 + used + w, home(p[i].id)->offset + w);
        }
        if (stack)
            offset -= n;
        else
            used += n;
    }
    fn->vararg_offset = offset;
}

static void call(struct instruction *in, const char *direct, unsigned *forced)
{
    struct value **args = in->args.data;
    size_t i;
    unsigned used = 0, w;
    unsigned long pushed = 0;
    bool stack = false;
    unsigned *sizes = calloc(in->args.count ? in->args.count : 1, sizeof(*sizes));
    unsigned long first = in->args.count;
    if (!sizes)
        fail("out of memory");
    for (i = 0; i < in->args.count; i++) {
        sizes[i] = forced ? forced[i] : width(args[i]);
        if (i >= in->stack_from || used + sizes[i] > 4)
            stack = true;
        if (stack && first == in->args.count)
            first = i;
        if (!stack)
            used += sizes[i];
        else
            pushed += sizes[i];
    }
    if (pushed > 32000 || fn->frame + pushed > 63980)
        fail("outgoing arguments exceed stack bank");
    /* Check before pushing so a failing call cannot write outside the stack. */
    {
        unsigned long id = ++label_id;
        emit("    LI X0, %lu\n    ADD X0, SP, X0\n    SLTU X1, X0, SP\n    BNE X1, R0, "
             "%s_call%lu_bad\n    LI X1, 64000\n    SLTU X1, X0, X1\n    BNE X1, R0, "
             "%s_call%lu_ok\n%s_call%lu_bad:\n    BRK\n%s_call%lu_ok:\n",
             pushed, mod->prefix, id, mod->prefix, id, mod->prefix, id, mod->prefix, id);
    }
    for (i = in->args.count; i > first; i--) {
        get(args[i - 1], 15, sizes[i - 1]);
        for (w = sizes[i - 1]; w; w--)
            emit("    STORE R%u, SB, SP\n    ADDI SP, SP, 1\n", 14 + w);
    }
    used = 0;
    for (i = 0; i < first; i++) {
        get(args[i], 15, sizes[i]);
        for (w = 0; w < sizes[i]; w++)
            move(7 + used + w, 15 + w);
        used += sizes[i];
    }
    if (!direct && in->a->kind != VFUNCTION && in->a->kind != VGLOBAL)
        get(in->a, 15, 2);
    immediate(13, pushed);
    if (direct)
        emit("    CALL %s\n", direct);
    else if (in->a->kind == VFUNCTION || in->a->kind == VGLOBAL)
        emit("    CALL %s\n", label(in->a->name));
    else
        emit("    JALR RA, T1, T0\n");
    if (pushed)
        emit("    ADDI SP, SP, -%lu\n", pushed);
    if (in->result >= 0) {
        unsigned n = slots(in->type);
        for (w = 0; w < n; w++)
            storeslot(returns[w], home((unsigned long)in->result)->offset + w);
    }
    free(sizes);
}

static void pointeradd(struct instruction *in)
{
    struct value **indices = in->args.data;
    unsigned n;
    unsigned long id = ++label_id;
    if (in->args.count != 1 || in->source_type->size != 1)
        fail("baseline GEP requires one cell index");
    components(in->a, 15);
    move(17, 16);
    immediate(16, 0);
    n = width(indices[0]);
    get(indices[0], 19, n > 2 ? 2 : n);
    if (n == 1)
        emit("    SRLI T5, T4, 15\n    SUB T5, R0, T5\n");
    helper("add", 2);
    emit("    BNE T1, R0, %s_gep%lu_slow\n    LI X0, 64000\n    SLTU X0, T0, X0\n    BNE X0, R0, "
         "%s_gep%lu_done\n%s_gep%lu_slow:\n",
         mod->prefix, id, mod->prefix, id, mod->prefix, id);
    move(21, 15);
    move(22, 16);
    immediate(19, 64000);
    immediate(20, 0);
    helper("sdiv", 2);
    emit("    ADD T2, T2, T0\n");
    move(15, 21);
    move(16, 22);
    immediate(19, 64000);
    immediate(20, 0);
    helper("srem", 2);
    emit("    SLT X0, T1, R0\n    BEQ X0, R0, %s_gep%lu_done\n    LI X0, 64000\n    ADD T0, T0, "
         "X0\n    ADDI T2, T2, -1\n%s_gep%lu_done:\n",
         mod->prefix, id, mod->prefix, id);
    move(16, 17);
    save(in->result, 15, 2);
}

static void intrinsic(struct instruction *in)
{
    const char *name = in->a->name;
    struct value **a = in->args.data;
    unsigned n = slots(in->type), w;
    if (!strncmp(name, "float.value.", 12) || !strncmp(name, "float.bits.", 11)) {
        if (in->args.count != 1 || in->result < 0)
            fail("invalid floating reinterpretation");
        get(a[0], 15, n);
        save(in->result, 15, n);
    } else if (!strncmp(name, "float.", 6)) {
        char helpername[96];
        const char *op;
        unsigned precision = 0, forced[2];
        if (!strncmp(name, "float.f32.", 10))
            precision = 2;
        else if (!strncmp(name, "float.f64.", 10))
            precision = 4;
        else
            fail("unknown floating intrinsic '%s'", name);
        op = name + 10;
        if (in->args.count != 1 && in->args.count != 2)
            fail("invalid floating intrinsic arity");
        forced[0] = precision;
        forced[1] = precision;
        if (!strcmp(op, "from_si") || !strcmp(op, "from_ui") || !strcmp(op, "from_f32"))
            forced[0] = 2;
        if (!strcmp(op, "from_i64") || !strcmp(op, "from_u64") || !strcmp(op, "from_f64"))
            forced[0] = 4;
        snprintf(helpername, sizeof(helpername), "__rcc_f%u_%s", precision * 16, op);
        call(in, helpername, forced);
    } else if (!strcmp(name, "va_start")) {
        if (!fn->vararg || in->args.count != 1)
            fail("invalid va_start");
        emit("    ADDI T0, FP, %ld\n", fn->vararg_offset);
        move(16, 28);
        memory(true, a[0], 15, 2);
    } else if (!strcmp(name, "va_arg")) {
        if (in->args.count != 1 || in->result < 0)
            fail("invalid va_arg");
        memory(false, a[0], 15, 2);
        move(17, 15);
        move(18, 16);
        concrete(18);
        for (w = 0; w < n; w++) {
            emit("    LOAD R%u, T3, T2\n", 7 + w);
            if (w + 1 < n)
                emit("    ADDI T2, T2, -1\n");
        }
        for (w = 0; w < n; w++)
            storeslot(7 + w, home((unsigned long)in->result)->offset + w);
        emit("    ADDI T0, T0, -%u\n", n);
        memory(true, a[0], 15, 2);
    } else
        fail("unsupported intrinsic '%s'", name);
}

static void instruction(struct instruction *in, unsigned long current)
{
    unsigned n = in->type ? slots(in->type) : 0, a, i;
    const char *op = in->op;
    int binary;
    line_number = in->line;
    emit("; IR line %lu: %s\n", in->line, op);
    if (!strcmp(op, "alloca")) {
        emit("    ADDI T0, FP, %lu\n", in->object);
        move(16, 28);
        save(in->result, 15, 2);
    } else if (!strcmp(op, "phi"))
        return;
    else if (!strcmp(op, "load")) {
        memory(false, in->b, 15, n);
        save(in->result, 15, n);
    } else if (!strcmp(op, "store")) {
        get(in->a, 15, n);
        memory(true, in->b, 15, n);
    } else if (!strcmp(op, "getelementptr"))
        pointeradd(in);
    else if (!strcmp(op, "call"))
        call(in, NULL, NULL);
    else if (!strcmp(op, "intrinsic"))
        intrinsic(in);
    else if (!strcmp(op, "ret")) {
        if (in->a) {
            n = slots(fn->result);
            get(in->a, 15, n);
            for (i = 0; i < n; i++)
                move(returns[i], 15 + i);
        }
        emit("    JAL R0, 0, %s_f%lu_return\n", mod->prefix, fn->serial);
    } else if (!strcmp(op, "br")) {
        if (!in->conditional)
            edge(current, in->yes);
        else {
            unsigned long id = ++label_id;
            get(in->a, 15, 1);
            emit("    BEQ T0, R0, %s_branch%lu_false\n", mod->prefix, id);
            edge(current, in->yes);
            emit("%s_branch%lu_false:\n", mod->prefix, id);
            edge(current, in->no);
        }
    } else if (!strcmp(op, "select")) {
        unsigned long id = ++label_id;
        get(in->a, 15, 1);
        emit("    BEQ T0, R0, %s_select%lu_false\n", mod->prefix, id);
        get(in->b, 15, n);
        emit("    BEQ R0, R0, %s_select%lu_done\n%s_select%lu_false:\n", mod->prefix, id,
             mod->prefix, id);
        get(in->c, 15, n);
        emit("%s_select%lu_done:\n", mod->prefix, id);
        save(in->result, 15, n);
    } else if (!strcmp(op, "ptrtolinear")) {
        components(in->a, 15);
        move(17, 15);
        move(15, 16);
        immediate(16, 0);
        immediate(19, 64000);
        immediate(20, 0);
        helper("mul", 2);
        move(19, 17);
        immediate(20, 0);
        helper("add", 2);
        save(in->result, 15, n);
    } else if (!strcmp(op, "sext") || !strcmp(op, "zext") || !strcmp(op, "trunc") ||
               !strcmp(op, "ptrtoint") || !strcmp(op, "inttoptr")) {
        a = width(in->a);
        if (!strcmp(op, "ptrtoint"))
            components(in->a, 15);
        else
            get(in->a, 15, n);
        if (!strcmp(op, "inttoptr")) {
            unsigned long id = ++label_id;
            emit("    OR SC, T0, T1\n    BNE SC, R0, %s_cast%lu_done\n    LI T1, "
                 "65533\n%s_cast%lu_done:\n",
                 mod->prefix, id, mod->prefix, id);
        }
        if (!strcmp(op, "sext") && in->a->kind == VNUMBER) {
            long long bits = (long long)in->a->bits;
            a = bits >= -32768LL && bits <= 32767LL             ? 1
                : bits >= -2147483648LL && bits <= 2147483647LL ? 2
                                                                : 4;
        }
        if (!strcmp(op, "sext") && a < n) {
            emit("    SRLI X0, R%u, 15\n    SUB X0, R0, X0\n", 14 + a);
            for (i = a; i < n; i++)
                move(15 + i, 11);
        }
        save(in->result, 15, n);
    } else if (!strcmp(op, "neg") || !strcmp(op, "not")) {
        get(in->a, 19, n);
        for (i = 0; i < n; i++)
            immediate(15 + i, !strcmp(op, "neg") ? 0 : 65535);
        helper(!strcmp(op, "neg") ? "sub" : "xor", n);
        save(in->result, 15, n);
    } else {
        for (binary = 0; binary < 23; binary++)
            if (!strcmp(op, binary_ops[binary]))
                break;
        if (binary == 23)
            fail("unsupported instruction '%s'", op);
        a = width(in->a);
        i = width(in->b);
        if (binary >= 13) {
            if (a > n)
                n = a;
            if (i > n)
                n = i;
        }
        get(in->a, 15, n);
        get(in->b, 19, n);
        helper(op, n);
        save(in->result, 15, slots(in->type));
    }
}

static void initvalue(struct symbol *s, struct type *t, struct value *v, unsigned long offset)
{
    unsigned long i;
    unsigned n;
    struct value **items;
    struct type **fields;
    if (!v)
        return;
    if (t->kind == TYARRAY) {
        if (v->kind != VARRAY && v->kind != VAGGREGATE)
            fail("array initializer must be a list");
        if (v->items.count > t->size / t->base->size)
            fail("too many array initializers");
        items = v->items.data;
        for (i = 0; i < v->items.count; i++)
            initvalue(s, t->base, items[i], offset + i * t->base->size);
    } else if (t->kind == TYSTRUCT) {
        if (v->kind != VAGGREGATE || v->items.count != t->fields.count)
            fail("invalid struct initializer");
        items = v->items.data;
        fields = t->fields.data;
        for (i = 0; i < t->fields.count; i++) {
            initvalue(s, fields[i], items[i], offset);
            offset += fields[i]->size;
        }
    } else {
        n = slots(t);
        if (v->kind == VNUMBER && !v->bits)
            return;
        get(v, 15, n);
        emit("    LI T4, %%data_offset(%s+%lu)\n    LI T5, %%data_bank(%s+%lu)\n    ADD T5, T5, "
             "GP\n",
             s->label, offset, s->label, offset);
        for (i = 0; i < n; i++) {
            emit("    STORE R%u, T5, T4\n", 15 + (unsigned)i);
            if (i + 1 < n)
                nextcell(19, 20);
        }
    }
}

static void metadata(void)
{
    size_t i;
    int op, n;
    bool comma = false;
    struct symbol *s = mod->symbols.data;
    emit("; rcc-link-v1 {\"banked_data\":true,\"code\":[\"__rcc_init_%s\"", mod->prefix);
    for (i = 0; i < mod->symbols.count; i++)
        if (s[i].defined && s[i].function)
            emit(",\"%s\"", s[i].label);
    for (op = 0; op < 23; op++)
        for (n = 1; n <= 4; n *= 2)
            if (helpers[op][n])
                emit(",\"%s_%s%d\"", mod->prefix, binary_ops[op], n);
    emit("],\"data\":{");
    for (i = 0; i < mod->symbols.count; i++)
        if (s[i].defined && !s[i].function) {
            emit("%s\"%s\":%lu", comma ? "," : "", s[i].label, s[i].type->size);
            comma = true;
        }
    emit("},\"roots\":[\"__rcc_init_%s\"", mod->prefix);
    for (i = 0; i < mod->symbols.count; i++)
        if (s[i].defined && s[i].function)
            emit(",\"%s\"", s[i].label);
    emit("],\"symbols\":{\"__rcc_init_%s\":[", mod->prefix);
    comma = false;
    for (i = 0; i < mod->symbols.count; i++)
        if (s[i].defined && !s[i].function) {
            emit("%s\"%s\"", comma ? "," : "", s[i].label);
            comma = true;
        }
    emit("]}}\n");
}

void emit_module(struct module *m, FILE *file)
{
    size_t i, j, k;
    struct symbol *s = m->symbols.data;
    struct function *f = m->functions.data;
    mod = m;
    out = file;
    label_id = 0;
    memset(helpers, 0, sizeof(helpers));
    for (i = 0; i < m->functions.count; i++)
        if (!f[i].external)
            layout(&f[i]);
    emit("; Portable Ripple baseline backend\n.data\n");
    for (i = 0; i < m->symbols.count; i++)
        if (s[i].defined && !s[i].function) {
            if (!s[i].type->size)
                fail("zero-sized global");
            emit("%s:\n.space %lu\n", s[i].label, s[i].type->size);
        }
    emit(".code\n__rcc_init_%s:\n", m->prefix);
    fn = NULL;
    for (i = 0; i < m->symbols.count; i++)
        if (s[i].defined && !s[i].function)
            initvalue(&s[i], s[i].type, s[i].init, 0);
    emit("    RET\n");
    for (i = 0; i < m->functions.count; i++)
        if (!f[i].external) {
            struct block *b = f[i].blocks.data;
            fn = &f[i];
            emit("%s:\n", label(fn->name));
            prologue(fn->frame);
            parameters();
            for (j = 0; j < fn->blocks.count; j++) {
                struct instruction **in = b[j].instructions.data;
                emit("%s_f%lu_b%lu:\n", m->prefix, fn->serial, b[j].id);
                for (k = 0; k < b[j].instructions.count; k++)
                    instruction(in[k], b[j].id);
            }
            emit("%s_f%lu_return:\n", m->prefix, fn->serial);
            epilogue();
        }
    emit_helpers(out, m->prefix, helpers);
    metadata();
    if (fflush(out) || ferror(out))
        fail("cannot write assembly");
}
