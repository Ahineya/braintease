/* Guest ELF64 -> RXE1 linker. All wire sizes count octets, not C16 cells. */
#ifndef RCC_X64_LINK_H
#define RCC_X64_LINK_H
#include <stdio.h>
#include <stddef.h>
#include "../native-format.h"
typedef unsigned long L32;
typedef unsigned long long L64;
#define LD_LIMIT 16777216UL

struct ld_section {
    L32 type, flags, offset, size, link, info, alignment, entsize, address;
    L32 resident, checksum;
};

struct ld_symbol {
    const char *name;
    L64 value, size;
    unsigned section, binding, type;
    struct ld_object *object;
    struct ld_symbol *definition;
    long helper;
    struct ld_symbol *next;
};

struct ld_object {
    const char *path, *source;
    unsigned char *data;
    L32 size, section_count, symbol_count;
    L32 start, source_size, checksum;
    struct ld_section *sections;
    struct ld_symbol *symbols;
};

struct ld_function {
    L32 token, arity;
    char *name;
    struct ld_symbol *symbol;
};

struct ld_fixup {
    L32 offset, addend;
};

struct ld_initializer {
    L32 pointer, width, function;
    L64 value;
};

struct ld_vector {
    void *data;
    size_t count, capacity;
};

extern const char *ld_path;
extern struct ld_vector ld_objects, ld_functions, ld_fixups;
extern L32 ld_entry, ld_code_size, ld_bindings, ld_helper_count;
extern struct ld_vector ld_initializers;
extern L32 ld_global_bank, ld_global_cells;
extern const char *ld_entry_symbol;
extern int ld_kernel_mode;
extern L32 ld_kernel_end, ld_kernel_readonly_end;
int ld_kernel_special(const char *);
void ld_kernel_layout(void);
void ld_kernel_write(FILE *);
L32 ld_kernel_got(L64, int);
void ld_efi(const char *, const char *, FILE *);
void ld_boot_media(const char *, FILE *);
unsigned ld_program_arity(void);
void ld_sdk(const char *, const char *);
void ld_error(const char *, ...);
void *ld_memory(size_t);
void *ld_append(struct ld_vector *, size_t);
L64 ld_read(const unsigned char *, unsigned);
void ld_patch(unsigned char *, L64, unsigned);
void ld_object(const char *);
struct ld_object *ld_read_object(const char *, L32, L32);
void ld_free_object(struct ld_object *);
const unsigned char *ld_section_data(struct ld_object *, struct ld_section *);
void ld_copy_sections(struct ld_object *);
unsigned char *ld_read_relocations(struct ld_object *, struct ld_section *);
void ld_image_allocate(void);
void ld_image_copy(L32, const unsigned char *, L32);
void ld_image_patch(L32, L64, unsigned);
void ld_image_write(FILE *);
void ld_input(const char *);
void ld_include(struct ld_object *);
void ld_object_symbols(struct ld_object *, void (*)(const char *, int, void *), void *);
void ld_metadata(const char *);
void ld_link(void);
void ld_write(FILE *);
struct ld_symbol *ld_local_symbol(struct ld_object *, const char *);
L64 ld_address(struct ld_symbol *);
void ld_modules(int);
void ld_modules_patch(void);
void ld_modules_relocation(struct ld_object *, unsigned, L64, unsigned);
#endif
