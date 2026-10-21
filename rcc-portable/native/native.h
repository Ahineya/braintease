/* Experimental checked C16 RCC IR -> QBE continuation lowering. */
#ifndef RCC_PORTABLE_NATIVE_H
#define RCC_PORTABLE_NATIVE_H
#include "../ripple/ripple.h"
#include "../native-format.h"

struct native_binding {
    char *name, *signature;
    unsigned kind, flags;
    unsigned long cells, offset;
};

struct native_var {
    unsigned long id;
    struct type *type;
    long parameter;
    size_t block, position;
};

struct native_point {
    struct instruction *instruction;
    unsigned char *after;
    unsigned long call_pc, checkpoint_pc;
};

struct native_block {
    struct block *block;
    struct vec points;
    unsigned char *in, *out;
};

struct native_function {
    struct function *function;
    struct vec variables, blocks;
    unsigned long cells, var_area;
    int kernel;
};

struct native_output {
    struct module *module;
    struct native_function *function;
    FILE *file;
    unsigned long temporary;
    int relocatable, no_start, kernel;
    struct vec bindings, metadata, constants;
    const char *constant_name;
    unsigned long records, entry_record, constant_offset;
};

long native_variable(struct native_function *, unsigned long);
void *native_scratch(size_t);
char *native_copy(const char *);
void native_scratch_reset(void);
long native_block(struct native_function *, unsigned long);
struct type *native_value_type(struct native_function *, struct value *);
void native_analyze(struct native_function *, struct function *);
void native_prune(struct function *);
void native_validate(struct native_function *);
void native_before(struct native_function *, struct instruction *, unsigned char *);
void native_emit(struct module *, FILE *);
void native_emit_relocatable(struct module *, FILE *);
void native_emit_library(struct module *, FILE *);
void native_emit_kernel(struct module *, FILE *);
void native_kernel_merge(struct module *, struct module *, unsigned long, int);
void native_kernel_reachable(struct module *);
void native_kernel_entry(struct native_output *);
int native_kernel_import(struct module *, const char *);
int native_process_service(const char *, unsigned *);
int native_service(struct module *, const char *);
void native_metadata(struct native_output *);
void native_metadata_write(struct native_output *);
char *native_signature(struct function *);
char *native_binding_value(struct native_output *, unsigned, const char *);
struct function *native_declaration(struct module *, const char *);
int native_global_reference(struct value *);
struct function *native_find_function(struct module *, const char *);
void native_assign(struct native_output *, struct instruction *, const char *);
unsigned native_operand_bits(struct native_output *, struct value *);
unsigned native_argument_cells(struct native_function *, struct value *);
int native_varargs(struct native_output *, struct instruction *);
void native_instruction(struct native_output *, struct instruction *);
void native_call(struct native_output *, struct native_point *);
void native_branch(struct native_output *, struct native_block *, struct native_point *);
char *native_value(struct native_output *, struct value *);
char *native_temporary(struct native_output *);
char *native_mask(struct native_output *, const char *, unsigned);
char *native_sign(struct native_output *, const char *, unsigned);
void native_float(struct native_output *, struct instruction *);
char *native_as_float(struct native_output *, char *, unsigned);
int native_conversion(struct native_output *, struct instruction *, unsigned, const char *);
#endif
