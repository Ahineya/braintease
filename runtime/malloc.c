/* A first-fit allocator over the existing banked heap.
 * All sizes are 16-bit C bytes (VM cells); size_t spans bank boundaries.
 * The VM and its Brainfuck implementation need no allocator instructions.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef BANK_SIZE
#define BANK_SIZE 64000
#endif

#define HEAP_START_BANK 5
#define HEAP_END_BANK 255
#define HEAP_CAPACITY ((size_t)(HEAP_END_BANK - HEAP_START_BANK + 1) * (size_t)BANK_SIZE)

struct heap_block {
    size_t size;
    struct heap_block *next;
    unsigned int available;
};

static struct heap_block *heap_head;

/* The pointer ABI is two cells: address, bank. */
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

static void init_heap(void) {
    if (heap_head == NULL) {
        heap_head = (struct heap_block *)make_fat_ptr(0, HEAP_START_BANK);
        heap_head->size = HEAP_CAPACITY - sizeof(struct heap_block);
        heap_head->next = NULL;
        heap_head->available = 1;
    }
}

static void *block_data(struct heap_block *block) {
    return (void *)((unsigned char *)block + sizeof(struct heap_block));
}

/* Adjacent list entries are physically adjacent in the linear banked arena. */
static void merge_next(struct heap_block *block) {
    struct heap_block *next = block->next;
    while (next != NULL && next->available) {
        block->size = block->size + sizeof(struct heap_block) + next->size;
        block->next = next->next;
        next = block->next;
    }
}

static void split_block(struct heap_block *block, size_t size) {
    size_t remaining = block->size - size;
    struct heap_block *tail;
    if (remaining <= sizeof(struct heap_block)) {
        return;
    }
    tail = (struct heap_block *)((unsigned char *)block_data(block) + size);
    tail->size = remaining - sizeof(struct heap_block);
    tail->next = block->next;
    tail->available = 1;
    block->size = size;
    block->next = tail;
    merge_next(tail);
}

void *malloc(size_t size) {
    struct heap_block *block;
    if (size == 0 || size > HEAP_CAPACITY - sizeof(struct heap_block)) {
        return NULL;
    }
    init_heap();
    block = heap_head;
    while (block != NULL) {
        if (block->available && block->size >= size) {
            split_block(block, size);
            block->available = 0;
            return block_data(block);
        }
        block = block->next;
    }
    return NULL;
}

void free(void *ptr) {
    struct heap_block *block;
    struct heap_block *previous = NULL;
    if (ptr == NULL) {
        return;
    }
    block = heap_head;
    while (block != NULL) {
        if (block_data(block) == ptr) {
            block->available = 1;
            merge_next(block);
            if (previous != NULL && previous->available) {
                merge_next(previous);
            }
            return;
        }
        previous = block;
        block = block->next;
    }
}

void *calloc(size_t nmemb, size_t size) {
    size_t total;
    void *ptr;
    if (nmemb == 0 || size == 0 || size > SIZE_MAX / nmemb) {
        return NULL;
    }
    total = nmemb * size;
    ptr = malloc(total);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    struct heap_block *block;
    void *replacement;
    size_t old_size;
    if (ptr == NULL) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    if (size > HEAP_CAPACITY - sizeof(struct heap_block)) {
        return NULL;
    }
    block = heap_head;
    while (block != NULL && block_data(block) != ptr) {
        block = block->next;
    }
    if (block == NULL) {
        return NULL;
    }
    old_size = block->size;
    if (block->size < size) {
        merge_next(block);
    }
    if (block->size >= size) {
        split_block(block, size);
        return ptr;
    }
    replacement = malloc(size);
    if (replacement == NULL) {
        return NULL;
    }
    memcpy(replacement, ptr, old_size);
    free(ptr);
    return replacement;
}
