/* Rust octets occupy C16 cells. These routines receive carrier values, never
 * Rust Layout/usize objects. Pointer arithmetic must preserve bank boundaries.
 * This is a prototype policy: at most 1,000,000 cells including overhead,
 * alignment <= 32768. Alignments above 512 stay inside a single bank: the
 * packed/linear pointer representations agree on alignment only through 512.
 */
#include <stdlib.h>
#include <string.h>

#define RUSTCELL_MAX_ALLOCATION 1000000UL
#define RUSTCELL_MAX_ALIGNMENT 32768UL
#define RUSTCELL_BANK_CELLS 64000UL

void *rustcell_alloc(unsigned long size, unsigned long align) {
    unsigned char *base, *start, *result;
    unsigned long offset, padding, overhead;
    if (!size || !align || align > RUSTCELL_MAX_ALIGNMENT ||
        (align & (align - 1)) != 0) return NULL;
    if (align > 512 && size > RUSTCELL_BANK_CELLS) return NULL;
    overhead = sizeof(void *) + (align > 512 ? RUSTCELL_BANK_CELLS - 1 : align - 1);
    if (size > RUSTCELL_MAX_ALLOCATION - overhead) return NULL;
    base = malloc(size + overhead);
    if (!base) return NULL;
    start = base + sizeof(void *);
    offset = (unsigned long)start & 65535UL;
    padding = (0UL - offset) & (align - 1);
    /* Rounding the packed offset into the bank gap would produce an invalid
     * pointer. The next bank's offset zero has every supported alignment. */
    if (offset + padding >= RUSTCELL_BANK_CELLS)
        padding = RUSTCELL_BANK_CELLS - offset;
    if (align > 512 && offset + padding + size > RUSTCELL_BANK_CELLS)
        padding = RUSTCELL_BANK_CELLS - offset;
    result = start + padding;
    *(void **)(result - sizeof(void *)) = base;
    return result;
}

void rustcell_dealloc(unsigned char *ptr, unsigned long size, unsigned long align) {
    (void)size; (void)align;
    if (ptr) free(*(void **)(ptr - sizeof(void *)));
}

void *rustcell_alloc_zeroed(unsigned long size, unsigned long align) {
    void *ptr = rustcell_alloc(size, align);
    if (ptr) memset(ptr, 0, size);
    return ptr;
}

void *rustcell_realloc(unsigned char *ptr, unsigned long old_size,
                       unsigned long align, unsigned long new_size) {
    void *next = rustcell_alloc(new_size, align);
    if (next) {
        memcpy(next, ptr, old_size < new_size ? old_size : new_size);
        rustcell_dealloc(ptr, old_size, align);
    }
    return next;
}

void rustcell_copy(unsigned char *destination, const unsigned char *source,
                   unsigned long count) { memmove(destination, source, count); }
void rustcell_fill(unsigned char *destination, unsigned short value,
                   unsigned long count) { memset(destination, value & 255, count); }

/* The intrinsic compares Rust octets, never the unused high bits of a cell. */
long rustcell_compare(const unsigned char *left, const unsigned char *right,
                      unsigned long count) {
    while (count--) {
        unsigned a = *left++ & 255, b = *right++ & 255;
        if (a != b) return a < b ? -1L : 1L;
    }
    return 0;
}
