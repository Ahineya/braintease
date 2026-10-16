#include <stack.h>
#include <stdlib.h>
#include <stdint.h>

#ifndef BANK_SIZE
#define BANK_SIZE 64000
#endif
#define STACK_LIMIT (BANK_SIZE - 10)

unsigned __rcc_stack_bank(void);
unsigned __rcc_stack_gp(void);
unsigned __rcc_stack_read(unsigned bank, unsigned offset);
void __rcc_stack_write(unsigned bank, unsigned offset, unsigned value);

union stack_pointer { void *pointer; unsigned words[2]; };

int rcc_stack_init(rcc_stack_context *context, unsigned first, unsigned count) {
    union stack_pointer bits;
    unsigned root = __rcc_stack_bank();
    unsigned bank;
    unsigned end;
    if (!context || !first || !count || BANK_SIZE < 128
        || (unsigned long)first + count > 65533UL
        || (root >= first && (unsigned long)root < (unsigned long)first + count)) return -1;
    if (__rcc_stack_read(root, STACK_LIMIT + 5)
        || __rcc_stack_read(root, STACK_LIMIT + 6)) return -1;
    bits.pointer = context;
    if (bits.words[1] == (unsigned)-1) bits.words[1] = __rcc_stack_gp();
    if (bits.words[1] == (unsigned)-2) bits.words[1] = root;
    context->free_bank = first;
    context->active_segments = 0;
    context->peak_segments = 0;
    context->root_bank = root;
    context->storage = NULL;
    end = first + count;
    bank = first;
    while (bank < end) {
        __rcc_stack_write(bank, STACK_LIMIT, bank + 1 == end ? 0 : bank + 1);
        __rcc_stack_write(bank, STACK_LIMIT + 5, bits.words[0]);
        __rcc_stack_write(bank, STACK_LIMIT + 6, bits.words[1]);
        bank++;
    }
    __rcc_stack_write(root, STACK_LIMIT + 5, bits.words[0]);
    __rcc_stack_write(root, STACK_LIMIT + 6, bits.words[1]);
    return 0;
}

int rcc_stack_reserve(rcc_stack_context *context, unsigned segments) {
    union stack_pointer bits;
    void *allocation;
    unsigned first;
    if (!context || !segments || segments > 249) return -1;
    allocation = malloc(((size_t)segments + 1UL) * (size_t)BANK_SIZE);
    if (!allocation) return -1;
    bits.pointer = allocation;
    first = bits.words[1] + (bits.words[0] != 0);
    if (rcc_stack_init(context, first, segments) != 0) {
        free(allocation);
        return -1;
    }
    context->storage = allocation;
    return 0;
}

int rcc_stack_destroy(rcc_stack_context *context) {
    union stack_pointer bits;
    unsigned root = __rcc_stack_bank();
    void *allocation;
    if (!context || context->root_bank != root || context->active_segments) return -1;
    bits.pointer = context;
    if (bits.words[1] == (unsigned)-1) bits.words[1] = __rcc_stack_gp();
    if (bits.words[1] == (unsigned)-2) bits.words[1] = root;
    if (__rcc_stack_read(root, STACK_LIMIT + 5) != bits.words[0]
        || __rcc_stack_read(root, STACK_LIMIT + 6) != bits.words[1]) return -1;
    allocation = context->storage;
    __rcc_stack_write(root, STACK_LIMIT + 5, 0);
    __rcc_stack_write(root, STACK_LIMIT + 6, 0);
    context->free_bank = 0;
    context->storage = NULL;
    free(allocation);
    return 0;
}
