#ifndef RCC_PORTABLE_ASM_TOOLS_H
#define RCC_PORTABLE_ASM_TOOLS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

struct array {
    void *data;
    size_t count, capacity;
};

void *array_append(struct array *, size_t);
void *allocate_zeroed(size_t);
char *copy_string(const char *);
void fatal(const char *, ...);
extern const char *source_file;
extern unsigned long source_line;
void put16(FILE *, unsigned);
void put32(FILE *, unsigned long);
unsigned get16(FILE *);
unsigned long get32(FILE *);
void putstr(FILE *, const char *);
char *getstr(FILE *);
FILE *open_input(const char *);
FILE *open_output(const char *);
void finish_output(FILE *);
void magic(FILE *, const char *);
/* Reuses caller-owned storage; the returned line is valid until the next read. */
char *readline(FILE *, struct array *);
char *trim(char *);
/* Every object is independently packed into 256-instruction chunks. Payload
 * occupies 0..253; an inserted non-linking jump at 254 crosses to the next
 * chunk. This makes object rebasing linear, without an in-memory PC map. */
#define BANK 64000UL
#define CHUNK 256U
/* Chunk alignment is a power of two. Use masks in per-instruction loops:
 * the baseline backend deliberately does not optimize division by constants. */
#define CHUNK_MASK 255UL
#define PAYLOAD 254U
#define RECORD 20UL
#define HEADER 28UL

/* Ripple instruction encoding. Add operand parsing in assemble.c for new opcodes. */
enum opcode {
    OP_HALT = 0x00,
    OP_ADD = 0x01,
    OP_SUB = 0x02,
    OP_AND = 0x03,
    OP_OR = 0x04,
    OP_XOR = 0x05,
    OP_SLL = 0x06,
    OP_SRL = 0x07,
    OP_SLT = 0x08,
    OP_SLTU = 0x09,
    OP_ADDI = 0x0a,
    OP_ANDI = 0x0b,
    OP_ORI = 0x0c,
    OP_XORI = 0x0d,
    OP_LI = 0x0e,
    OP_SLLI = 0x0f,
    OP_SRLI = 0x10,
    OP_LOAD = 0x11,
    OP_STORE = 0x12,
    OP_JAL = 0x13,
    OP_JALR = 0x14,
    OP_BEQ = 0x15,
    OP_BNE = 0x16,
    OP_BLT = 0x17,
    OP_BGE = 0x18,
    OP_BRK = 0x19,
    OP_MUL = 0x1a,
    OP_DIV = 0x1b,
    OP_MOD = 0x1c,
    OP_MULI = 0x1d,
    OP_DIVI = 0x1e,
    OP_MODI = 0x1f,
    OP_LOADC = 0x20,
    OP_STORC = 0x21,
    OP_IRET = 0x22,
    OP_CSRR = 0x23,
    OP_CSRW = 0x24,
    OP_SYSCALL = 0x25,
    OP_ENTERU = 0x26,
    OP_NOP = 0x42
};

/* RPO1 wire values: append new kinds without renumbering existing ones.
 * RJAL patches the bank+offset pair in a jump. The projection kinds patch
 * a single LI immediate. RDATA is the legacy, single-bank data address. */
enum relocation_kind { RNONE, RJAL, RCBANK, RCOFF, RDBANK, RDOFF, RDATA };

enum target_kind { TNONE, TLOCALCODE, TLOCALDATA, TEXTERNAL };

struct record {
    /* a/b/c are the three encoded instruction words, not virtual registers. */
    unsigned op, a, b, c, kind, target;
    /* Local section offset, or an index into the owning object's externs. */
    unsigned long value;
    long addend;
};

void put_record(FILE *, struct record *);
void get_record(FILE *, struct record *);

struct definition {
    char *name;
    unsigned kind;
    unsigned long value;
};

struct object {
    const char *path;
    /* Input file offsets, section sizes, then assigned output section bases.
     * Archive members share a path but have distinct body/code_at/data_at. */
    unsigned long body, code_at, data_at, ncode, ndata, base, database;
    bool banked, selected;
    struct array defs, externs;
};
/* Header ownership lasts until process exit; payloads stay on disk. */
struct object *read_object(FILE *, const char *);
int assembler(int, char **);
int linker(int, char **);
#endif
