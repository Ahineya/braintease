// malloc.c - Bump allocator for heap memory
// Allocations may span bank boundaries; GEP handles indexing across banks.
// BANK_SIZE is the VM bank size in words, injected at compile time (-D BANK_SIZE=...).

#include <stddef.h>
#include <string.h>

#ifndef BANK_SIZE
#define BANK_SIZE 64000
#endif

#define HEAP_START_BANK 5
#define HEAP_END_BANK 255

static unsigned int current_heap_bank = HEAP_START_BANK;
static unsigned int current_heap_offset = 0;
static unsigned char heap_initialized = 0;

static void init_heap(void) {
    if (!heap_initialized) {
        current_heap_bank = HEAP_START_BANK;
        current_heap_offset = 0;
        heap_initialized = 1;
    }
}

/* A fat pointer is two words (addr, bank). Inline asm `=r` only captures
 * one register and then the compiler overwrites RV0/RV1 from an
 * uninitialized local, so every malloc after the first can return garbage.
 * Write both words through a union so `return` loads a real fat pointer. */
union heap_fat_ptr {
    void *p;
    unsigned int w[2];
};

static void *make_fat_ptr(unsigned int addr, unsigned int bank) {
    union heap_fat_ptr fp;
    fp.w[0] = addr;
    fp.w[1] = bank;
    return fp.p;
}

void *malloc(unsigned int size) {
    unsigned int start_bank;
    unsigned int start_off;
    unsigned int remaining;
    unsigned int extra;
    unsigned int extra_banks;
    unsigned int end_bank;
    unsigned int end_off;
    unsigned int last_used;
    unsigned int bank_words;

    init_heap();

    if (size == 0) {
        return NULL;
    }

    bank_words = BANK_SIZE;

    if (current_heap_bank > (unsigned int)HEAP_END_BANK) {
        return NULL;
    }

    start_bank = current_heap_bank;
    start_off = current_heap_offset;
    /* Compiler null checks only the address word, so offset 0 is NULL. */
    if (start_off == 0) {
        start_off = 1;
    }
    remaining = bank_words - start_off;

    if (size < remaining) {
        end_bank = start_bank;
        end_off = start_off + size;
    } else if (size == remaining) {
        end_bank = start_bank + 1;
        end_off = 0;
    } else {
        extra = size - remaining;
        extra_banks = extra / bank_words;
        end_off = extra % bank_words;
        end_bank = start_bank + 1 + extra_banks;
    }

    if (end_off == 0) {
        last_used = end_bank - 1;
    } else {
        last_used = end_bank;
    }

    if (last_used > (unsigned int)HEAP_END_BANK) {
        return NULL;
    }

    current_heap_bank = end_bank;
    current_heap_offset = end_off;
    return make_fat_ptr(start_off, start_bank);
}

void free(void *ptr) {
    (void)ptr;
}

void *calloc(unsigned int nmemb, unsigned int size) {
    unsigned int total;
    void *ptr;

    if (nmemb == 0 || size == 0) {
        return NULL;
    }

    total = nmemb * size;
    if (total / nmemb != size) {
        return NULL;
    }

    ptr = malloc(total);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *realloc(void *ptr, unsigned int size) {
    if (ptr == NULL) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    return NULL;
}
