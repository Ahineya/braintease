/* Portable x86_64 assembler. Wire quantities count octets, never C cells. */
#ifndef RCC_PORTABLE_X64_H
#define RCC_PORTABLE_X64_H
#include <stddef.h>
#include <stdio.h>

typedef unsigned long U32;
typedef unsigned long long U64;
typedef long long I64;
/* The bootstrap SDK does not declare this C99 conversion yet. */
U64 strtoull(const char *, char **, int);

struct vector {
    void *data;
    size_t count, capacity;
};

struct buffer {
    unsigned char *data;
    size_t size, capacity;
};

struct section {
    const char *name;
    struct buffer bytes;
    U32 alignment, size;
};

struct symbol {
    char *name;
    unsigned section, global, local, type, defined;
    U32 value, size, index, name_at;
};

struct x64_expression {
    long symbol;
    U64 addend;
};

struct relocation {
    unsigned section, type;
    U32 offset;
    struct x64_expression value;
};

struct x64_operand {
    unsigned kind, width;
    int reg, base, index, scale, xmm, indirect;
    struct x64_expression value;
};

enum { OREG, OMEM, OIMM, OLABEL };

enum { TEXT, DATA, BSS, RODATA, NSECTIONS };
extern struct section x64_sections[NSECTIONS];
extern struct vector x64_symbols, x64_relocations;
extern unsigned x64_current_section;
extern const char *x64_source_path;
extern U32 x64_source_line;
void x64_error(const char *, ...);
void *x64_append(struct vector *, size_t);
void *x64_memory(size_t);
char *x64_copy(const char *);
char *x64_trim(char *);
void x64_byte(struct buffer *, unsigned);
void x64_number(struct buffer *, U64, unsigned);
void x64_emit(unsigned);
void x64_emit_number(U64, unsigned);
void x64_emit_value(struct x64_expression, unsigned, unsigned);
long x64_symbol_id(const char *);
struct x64_expression x64_expression(char *);
void x64_instruction(char *, struct x64_operand *, unsigned);
struct x64_operand x64_operand(char *);
void x64_assemble(FILE *);
void x64_write_object(FILE *);
#endif
