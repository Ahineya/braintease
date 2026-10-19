#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "tools.h"

/* Encoding order matches enum opcode; these tables are shared by every line. */
static const char *const register_names[32] = {"R0", "PC", "PCB", "RA", "RAB", "RV0", "RV1", "A0",
                                               "A1", "A2", "A3",  "X0", "X1",  "X2",  "X3",  "T0",
                                               "T1", "T2", "T3",  "T4", "T5",  "T6",  "T7",  "S0",
                                               "S1", "S2", "S3",  "SC", "SB",  "SP",  "FP",  "GP"};

static const char *const instruction_names[] = {
    "HALT", "ADD",  "SUB",   "AND",   "OR",   "XOR",  "SLL",  "SRL",     "SLT",   "SLTU",
    "ADDI", "ANDI", "ORI",   "XORI",  "LI",   "SLLI", "SRLI", "LOAD",    "STORE", "JAL",
    "JALR", "BEQ",  "BNE",   "BLT",   "BGE",  "BRK",  "MUL",  "DIV",     "MOD",   "MULI",
    "DIVI", "MODI", "LOADC", "STORC", "IRET", "CSRR", "CSRW", "SYSCALL", "ENTERU"};

/* Generated private labels share a 24-character compilation-unit prefix. */
struct symbol_prefix {
    struct symbol_prefix *next;
    char text[25];
};

struct assembly_symbol {
    char *text;
    struct symbol_prefix *prefix;
    unsigned long value, index;
    unsigned kind;
    bool defined;
    struct assembly_symbol *next;
};
/* Symbols belong to this assembly unit and survive all three scans. */
static struct assembly_symbol *symbol_buckets[4096];
static struct array symbols;
static unsigned long code_count, data_count, data_extent;
static bool data_section, emit_records;
static FILE *object_output;
static struct symbol_prefix *prefixes;

union symbol_cell {
    void *pointer;
    unsigned long long bits;
    double number;
};
static union symbol_cell *symbol_arena;
static size_t symbol_left;

/* Symbols survive all scans and are released together when the tool exits. */
static void *symbol_storage(size_t n)
{
    size_t cells = n / sizeof(*symbol_arena) + (n % sizeof(*symbol_arena) != 0);
    void *p;
    if (cells > symbol_left) {
        size_t capacity = cells > 4096 ? cells : 4096;
        symbol_arena = allocate_zeroed(capacity * sizeof(*symbol_arena));
        symbol_left = capacity;
    }
    p = symbol_arena;
    symbol_arena += cells;
    symbol_left -= cells;
    return p;
}

static struct symbol_prefix *generated_prefix(const char *text)
{
    struct symbol_prefix *prefix;
    size_t i;
    if (strncmp(text, "__rcp_", 6) || strlen(text) <= 24 || text[14] != '_' || text[23] != '_')
        return NULL;
    for (i = 6; i < 23; i++)
        if (i != 14 && !isxdigit((unsigned char)text[i]))
            return NULL;
    for (prefix = prefixes; prefix; prefix = prefix->next)
        if (!strncmp(prefix->text, text, 24))
            return prefix;
    prefix = symbol_storage(sizeof(*prefix));
    memcpy(prefix->text, text, 24);
    prefix->next = prefixes;
    prefixes = prefix;
    return prefix;
}

static unsigned symbol_hash(const char *s)
{
    unsigned n = 0;
    for (; *s; s++)
        n = (n * 33U + (unsigned char)*s) & 4095U;
    return n;
}

static struct assembly_symbol *intern_symbol(const char *text)
{
    unsigned h = symbol_hash(text);
    struct assembly_symbol *n;
    struct symbol_prefix *prefix;
    if (!*text)
        fatal("empty symbol name");
    prefix = generated_prefix(text);
    if (prefix)
        text += 24;
    for (n = symbol_buckets[h]; n; n = n->next)
        if (n->prefix == prefix && !strcmp(n->text, text))
            return n;
    if (emit_records)
        fatal("symbol appeared after first pass: %s", text);
    n = symbol_storage(sizeof(*n) + strlen(text) + 1);
    n->text = (char *)(n + 1);
    strcpy(n->text, text);
    n->prefix = prefix;
    n->next = symbol_buckets[h];
    symbol_buckets[h] = n;
    *(struct assembly_symbol **)array_append(&symbols, sizeof(n)) = n;
    return n;
}

static long parse_integer(const char *s)
{
    char *end;
    long n;
    errno = 0;
    n = strtol(s, &end, 0);
    if (!*s || *end || errno == ERANGE)
        fatal("invalid integer '%s'", s);
    return n;
}

static unsigned parse_register(const char *s)
{
    char b[16];
    size_t i;

    if (strlen(s) >= sizeof(b))
        fatal("invalid register '%s'", s);
    for (i = 0; s[i]; i++)
        b[i] = (char)toupper((unsigned char)s[i]);
    b[i] = 0;
    if (!strcmp(b, "ZR"))
        return 0;
    if (b[0] == 'R' && isdigit((unsigned char)b[1])) {
        long n = parse_integer(b + 1);
        if (n >= 0 && n < 32)
            return (unsigned)n;
    }
    for (i = 0; i < 32; i++)
        if (!strcmp(b, register_names[i]))
            return (unsigned)i;
    fatal("invalid register '%s'", s);
    return 0;
}

static void emit_record(struct record *r)
{
    if (emit_records)
        put_record(object_output, r);
    code_count++;
}

static void emit_nop(void)
{
    struct record r = {0};
    r.op = OP_NOP;
    emit_record(&r);
}

/* Keep a conditional branch and its absolute jump in the same chunk. */
static void reserve_payload(unsigned count)
{
    struct record bridge = {0};
    unsigned at = (unsigned)((code_count & CHUNK_MASK));
    if (at + count <= PAYLOAD)
        return;
    while ((code_count & CHUNK_MASK) < PAYLOAD)
        emit_nop();
    bridge.op = OP_JAL;
    bridge.kind = RJAL;
    bridge.target = TLOCALCODE;
    bridge.value = (code_count & ~CHUNK_MASK) + CHUNK;
    emit_record(&bridge);
    emit_nop();
}

static void parse_reference(struct record *r, char *text, unsigned kind)
{
    struct assembly_symbol *name;
    char *p, *end;
    long displacement = 0;
    text = trim(text);
    if (kind == RDATA && (*text == '-' || isdigit((unsigned char)*text))) {
        long n = parse_integer(text);
        if (n < -65535L || n > 65535L)
            fatal("immediate is outside one word");
        r->b = (unsigned)n;
        return;
    }
    if (kind == RDATA && *text == '%') {
        if (!strncmp(text, "%data_offset(", 13)) {
            kind = RDOFF;
            text += 13;
        } else if (!strncmp(text, "%data_bank(", 11)) {
            kind = RDBANK;
            text += 11;
        } else if (!strncmp(text, "%offset(", 8)) {
            kind = RCOFF;
            text += 8;
        } else if (!strncmp(text, "%bank(", 6)) {
            kind = RCBANK;
            text += 6;
        } else
            fatal("unknown relocation expression '%s'", text);
        end = strrchr(text, ')');
        if (!end || end[1])
            fatal("invalid relocation expression");
        *end = 0;
    }
    if (kind == RDOFF || kind == RDBANK) {
        p = strchr(text, '+');
        if (!p) {
            p = strchr(text + 1, '-');
        }
        if (p) {
            displacement = parse_integer(p[0] == '+' ? p + 1 : p);
            *p = 0;
        }
    }
    name = intern_symbol(text);
    r->kind = kind;
    r->addend = displacement;
    if (name->defined) {
        r->target = name->kind ? TLOCALDATA : TLOCALCODE;
        r->value = name->value;
    } else {
        r->target = TEXTERNAL;
        r->value = name->index;
    }
    if (emit_records && name->defined) {
        if ((kind == RJAL || kind == RCOFF || kind == RCBANK) && name->kind)
            fatal("code relocation names data '%s'", text);
        if ((kind == RDOFF || kind == RDBANK || kind == RDATA) && !name->kind)
            fatal("data relocation names code '%s'", text);
    }
}

static void parse_instruction(char *op, char *args)
{
    char *a[4];
    unsigned n = 0, i;
    char *p = args;
    struct record r = {0};

    for (i = 0; op[i]; i++)
        op[i] = (char)toupper((unsigned char)op[i]);
    while (*p) {
        if (n == 4)
            fatal("too many operands");
        a[n++] = trim(p);
        p = strchr(p, ',');
        if (!p)
            break;
        *p++ = 0;
        a[n - 1] = trim(a[n - 1]);
    }
    if (!strcmp(op, "CALL")) {
        if (n != 1)
            fatal("CALL takes one operand");
        r.op = OP_JAL;
        r.a = 3;
        parse_reference(&r, a[0], RJAL);
        reserve_payload(1);
        emit_record(&r);
        return;
    }
    if (!strcmp(op, "RET")) {
        if (n)
            fatal("RET takes no operands");
        r.op = OP_JALR;
        r.b = 4;
        r.c = 3;
        reserve_payload(1);
        emit_record(&r);
        return;
    }
    if (!strcmp(op, "MOVE")) {
        if (n != 2)
            fatal("MOVE takes two operands");
        r.op = OP_ADD;
        r.a = parse_register(a[0]);
        r.b = parse_register(a[1]);
        reserve_payload(1);
        emit_record(&r);
        return;
    }
    if (!strcmp(op, "NOP")) {
        if (n)
            fatal("NOP takes no operands");
        reserve_payload(1);
        emit_nop();
        return;
    }
    for (i = 0; i < sizeof(instruction_names) / sizeof(instruction_names[0]); i++)
        if (!strcmp(op, instruction_names[i]))
            break;
    if (i == sizeof(instruction_names) / sizeof(instruction_names[0]))
        fatal("unknown instruction '%s'", op);
    r.op = i;
    if (i == OP_HALT || i == OP_BRK || i == OP_IRET || i == OP_SYSCALL) {
        if (n)
            fatal("instruction takes no operands");
        reserve_payload(1);
        emit_record(&r);
        return;
    }
    if (i == OP_LI || i == OP_CSRR || i == OP_CSRW) {
        if (n != 2)
            fatal("instruction requires two operands");
        r.a = parse_register(a[0]);
        parse_reference(&r, a[1], RDATA);
        reserve_payload(1);
        emit_record(&r);
        return;
    }
    if (n != 3)
        fatal("instruction requires three operands");
    r.a = parse_register(a[0]);
    if (i >= OP_BEQ && i <= OP_BGE) {
        r.b = parse_register(a[1]);
        if (isdigit((unsigned char)*a[2]) || *a[2] == '-')
            fatal("numeric branch displacements are unsupported; use a label");
        parse_reference(&r, a[2], RJAL);
        if (r.a == r.b) {
            if (i == OP_BNE || i == OP_BLT) {
                reserve_payload(1);
                emit_nop();
                return;
            }
            r.op = OP_JAL;
            r.a = 0;
            r.b = 0;
            r.c = 0;
            reserve_payload(1);
            emit_record(&r);
            return;
        }
        {
            struct record inv = {0};
            inv.op = i == OP_BEQ ? OP_BNE : i == OP_BNE ? OP_BEQ : i == OP_BLT ? OP_BGE : OP_BLT;
            inv.a = r.a;
            inv.b = r.b;
            inv.c = 2;
            reserve_payload(2);
            emit_record(&inv);
            r.op = OP_JAL;
            r.a = 0;
            r.b = 0;
            r.c = 0;
            emit_record(&r);
        }
        return;
    }
    if (i == OP_JAL) {
        long bank = parse_integer(a[1]);
        if (bank)
            fatal("symbolic JAL bank must be zero");
        parse_reference(&r, a[2], RJAL);
    } else if ((i >= OP_ADD && i <= OP_SLTU) || i == OP_JALR || i == OP_MUL || i == OP_DIV ||
               i == OP_MOD || i == OP_LOADC || i == OP_STORC || i == OP_ENTERU || i == OP_LOAD ||
               i == OP_STORE) {
        r.b = parse_register(a[1]);
        r.c = parse_register(a[2]);
    } else {
        long imm;
        r.b = parse_register(a[1]);
        imm = parse_integer(a[2]);
        if (imm < -65535L || imm > 65535L)
            fatal("immediate is outside one word");
        r.c = (unsigned)imm;
    }
    reserve_payload(1);
    emit_record(&r);
}

static void parse_line(char *raw, bool data_output)
{
    char *s = trim(raw), *p, *args;
    if (!*s)
        return;
    p = strchr(s, ':');
    if (p) {
        struct assembly_symbol *n;
        *p = 0;
        n = intern_symbol(trim(s));
        if (!emit_records && !data_output) {
            if (n->defined)
                fatal("duplicate symbol '%s'", s);
            n->defined = true;
            n->kind = data_section ? 1 : 0;
            n->value = data_section ? data_count : code_count;
        }
        s = trim(p + 1);
        if (!*s)
            return;
    }
    p = s;
    while (*p && !isspace((unsigned char)*p))
        p++;
    if (*p) {
        *p++ = 0;
        args = trim(p);
    } else
        args = p;
    if (*s == '.') {
        if (!strcmp(s, ".data")) {
            data_section = true;
            return;
        }
        if (!strcmp(s, ".code") || !strcmp(s, ".text")) {
            data_section = false;
            return;
        }
        if (!strcmp(s, ".space")) {
            long n = parse_integer(args), i;
            if (!data_section || n < 0 || data_count + (unsigned long)n >= 16UL * BANK)
                fatal("invalid data reservation");
            if (data_output)
                for (i = 0; i < n; i++)
                    putc(0, object_output);
            data_count += (unsigned long)n;
            return;
        }
        if (!strcmp(s, ".word")) {
            if (!data_section)
                fatal(".word requires .data");
            do {
                long n;
                p = strchr(args, ',');
                if (p)
                    *p = 0;
                n = parse_integer(trim(args));
                if (n < 0 || n > 255)
                    fatal("RXE initialized data is restricted to octets; initialize wide cells in "
                          "code");
                if (data_output)
                    putc((int)n, object_output);
                data_count++;
                if (p)
                    args = p + 1;
            } while (p);
            return;
        }
        fatal("unsupported directive '%s'", s);
    }
    if (data_section)
        fatal("instruction in data section");
    if (!data_output)
        parse_instruction(s, args);
}

static void scan_assembly(FILE *f, bool data_output)
{
    struct array line_buffer = {0};
    char *s;
    source_line = 0;
    code_count = 0;
    data_count = 0;
    data_section = false;
    if (fseek(f, 0, SEEK_SET))
        fatal("assembly input must be seekable");
    while ((s = readline(f, &line_buffer)))
        parse_line(s, data_output);
    free(line_buffer.data);
    if (ferror(f))
        fatal("cannot read assembly input");
    if (!data_output && code_count)
        while ((code_count & CHUNK_MASK))
            emit_nop();
}

static bool symbol_exported(struct assembly_symbol *n)
{
    return !n->prefix && strncmp(n->text, "__rcp_", 6) != 0;
}

static void write_symbol(struct assembly_symbol *n)
{
    if (!n->prefix) {
        putstr(object_output, n->text);
        return;
    }
    /* An unresolved private reference still needs its complete spelling. */
    put32(object_output, (unsigned long)strlen(n->text) + 24);
    fputs(n->prefix->text, object_output);
    fputs(n->text, object_output);
}

int assembler(int argc, char **argv)
{
    const char *input = NULL, *dest = NULL;
    FILE *f;
    size_t i;
    unsigned long ns = 0, ne = 0;
    struct assembly_symbol **all;
    for (i = 1; i < (size_t)argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < (size_t)argc)
            dest = argv[++i];
        else if (argv[i][0] == '-' || input)
            fatal("usage: rcc-as [-o OBJECT] ASSEMBLY");
        else
            input = argv[i];
    }
    if (!input || !dest || !strcmp(input, dest))
        fatal("assembly input and separate -o output are required");
    source_file = input;
    f = open_input(input);
    /* Scan 1 assigns addresses and collects forward references. */
    scan_assembly(f, false);
    data_extent = data_count;
    all = symbols.data;
    for (i = 0; i < symbols.count; i++) {
        if (all[i]->defined) {
            if (symbol_exported(all[i]))
                ns++;
        } else
            all[i]->index = ne++;
    }
    object_output = open_output(dest);
    fputs("RPO1", object_output);
    put32(object_output, 1);
    put32(object_output, code_count);
    put32(object_output, data_extent);
    put32(object_output, ns);
    put32(object_output, ne);
    put32(object_output, 1);
    for (i = 0; i < symbols.count; i++)
        if (all[i]->defined && symbol_exported(all[i])) {
            write_symbol(all[i]);
            put16(object_output, all[i]->kind);
            put32(object_output, all[i]->value);
        }
    for (i = 0; i < symbols.count; i++)
        if (!all[i]->defined)
            write_symbol(all[i]);
    /* Scan 2 emits relocated instruction records; scan 3 emits data cells. */
    emit_records = true;
    scan_assembly(f, false);
    scan_assembly(f, true);
    fclose(f);
    finish_output(object_output);
    return 0;
}
