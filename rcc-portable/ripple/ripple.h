#ifndef RCC_PORTABLE_RIPPLE_H
#define RCC_PORTABLE_RIPPLE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

struct vec {
    void *data;
    size_t count, capacity;
};

void *append(struct vec *, size_t);
/* Zeroed module-lifetime storage; callers must not free individual objects. */
void *allocate(size_t);
/* Immutable interned text, with the same module lifetime. */
char *duplicate(const char *);
void fail(const char *, ...);
extern const char *filename;
extern unsigned long line_number;

struct type {
    int kind;
    unsigned long size;
    unsigned bits;
    struct type *base;
    struct vec fields;
};

enum { TYVOID, TYINT, TYFLOAT, TYPOINTER, TYARRAY, TYSTRUCT };

struct value {
    int kind;
    unsigned long id;
    unsigned long long bits;
    char *name;
    struct value *base;
    long offset;
    int bank;
    struct vec items;
};

enum { VNUMBER, VTEMP, VGLOBAL, VFUNCTION, VFAT, VOFFSET, VARRAY, VAGGREGATE, VUNDEF };

/* Compile-time tags must remain distinct from every concrete bank, including 0. */
enum { BGLOBAL = -1, BSTACK = -2, BMIXED = -3, BNULL = -4 };

struct instruction {
    char *op;
    long result;
    struct type *type, *source_type;
    struct value *a, *b, *c;
    struct vec args, labels;
    unsigned long yes, no, stack_from, line;
    bool conditional;
    unsigned long object, shadow;
};

struct block {
    unsigned long id;
    struct vec instructions;
};

struct home {
    struct type *type;
    unsigned long offset;
    bool defined;
};

struct parameter {
    unsigned long id;
    struct type *type;
};

struct function {
    char *name;
    struct type *result;
    bool external, vararg;
    struct vec parameters, blocks, homes;
    unsigned long frame, serial;
    long vararg_offset;
};

struct symbol {
    char *name, *label;
    bool internal, function, defined, shared;
    struct type *type;
    struct value *init;
};

struct module {
    struct vec functions, symbols;
    char prefix[32];
    unsigned long serial;
};
struct module *parse(FILE *, const char *);
/* The same grammar/layout, with explicit backend target validation. */
struct module *parse_target(FILE *, const char *, const char *);
struct symbol *symbol(struct module *, const char *);
unsigned int slots(struct type *);
void emit_module(struct module *, FILE *);
void emit_helpers(FILE *, const char *, bool used[24][5]);
extern const char *binary_ops[24];
#endif
