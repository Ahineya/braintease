/* Segregated free lists over the existing banked heap.
 * All sizes are 16-bit C bytes (VM cells); size_t spans bank boundaries.
 * The VM and its Brainfuck implementation need no allocator instructions.
 * Only free blocks are searched. Physical links make free/realloc independent
 * of the number of live allocations. Headers may themselves cross banks.
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
    struct heap_block *previous;
    struct heap_block *free_next;
    struct heap_block *free_previous;
    /* Zero for an allocated block, otherwise its free-list index plus one. */
    unsigned int free_bin;
};

/* Exact small sizes, then doubling upper bounds through the entire heap.
 * No bit scanning or division is needed on the Brainfuck CPU. The largest
 * supported arena (251 * 65535 cells) fits the final 16777216-cell bucket.
 */
#define SMALL_BINS 32
#define BIN_COUNT 51
static struct heap_block *free_bins[BIN_COUNT];
static unsigned int first_free = BIN_COUNT;
static unsigned int heap_initialized;
extern void *__rcc_heap_start(void);

/* The pointer ABI is two cells: address, bank. */
union heap_fat_ptr {
    void *p;
    unsigned int w[2];
};

static unsigned int size_bin(size_t size) {
    unsigned int index = SMALL_BINS;
    size_t limit = SMALL_BINS * 2;
    if (size <= SMALL_BINS) {
        return (unsigned int)size - 1;
    }
    while (size > limit) {
        limit = limit + limit;
        index++;
    }
    return index;
}

static void remove_free(struct heap_block *block) {
    unsigned int index = block->free_bin - 1;
    struct heap_block *next = block->free_next;
    struct heap_block *previous = block->free_previous;
    if (previous != NULL) previous->free_next = next;
    else free_bins[index] = next;
    if (next != NULL) next->free_previous = previous;
    block->free_bin = 0;
}

static void insert_free(struct heap_block *block) {
    unsigned int index = size_bin(block->size);
    struct heap_block *next = free_bins[index];
    block->free_bin = index + 1;
    block->free_next = next;
    block->free_previous = NULL;
    if (next != NULL) next->free_previous = block;
    free_bins[index] = block;
    if (index < first_free) first_free = index;
}

static void init_heap(void) {
    union heap_fat_ptr start;
    struct heap_block *block;
    if (heap_initialized) return;
    heap_initialized = 1;
    start.p = __rcc_heap_start();
    /* Large global images can leave no banks in the compatibility heap. */
    if (start.w[1] > HEAP_END_BANK) return;
    block = start.p;
    block->size = (size_t)(HEAP_END_BANK - start.w[1] + 1) * BANK_SIZE - sizeof(struct heap_block);
    block->next = NULL;
    block->previous = NULL;
    insert_free(block);
}

static void *block_data(struct heap_block *block) {
    return (void *)((unsigned char *)block + sizeof(struct heap_block));
}

/* Coalescing removes the neighbor from its bin before changing any sizes.
 * The surviving block is not in a free list; its caller inserts it afterward.
 * Eager coalescing guarantees there is at most one free neighbor per side.
 */
static void merge_next(struct heap_block *block) {
    struct heap_block *next = block->next;
    if (next != NULL && next->free_bin != 0) {
        remove_free(next);
        block->size = block->size + sizeof(struct heap_block) + next->size;
        block->next = next->next;
        if (block->next != NULL) block->next->previous = block;
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
    tail->previous = block;
    if (tail->next != NULL) tail->next->previous = tail;
    block->size = size;
    block->next = tail;
    merge_next(tail);
    insert_free(tail);
}

void *malloc(size_t size) {
    struct heap_block *block;
    unsigned int index;
    if (size == 0 || size > HEAP_CAPACITY - sizeof(struct heap_block)) {
        return NULL;
    }
    init_heap();
    index = size_bin(size);
    if (index < first_free) index = first_free;
    while (index < BIN_COUNT) {
        block = free_bins[index];
        while (block != NULL) {
            if (block->size >= size) {
                remove_free(block);
                split_block(block, size);
                return block_data(block);
            }
            block = block->free_next;
        }
        /* first_free may lag behind after coalescing, but never skips a
         * nonempty smaller bin. Advancing it here avoids scanning all the
         * empty small buckets on each allocation from the large tail.
         */
        if (index == first_free && free_bins[index] == NULL) first_free++;
        index++;
    }
    return NULL;
}

void free(void *ptr) {
    struct heap_block *block;
    struct heap_block *previous;
    if (ptr == NULL) {
        return;
    }
    block = (struct heap_block *)((unsigned char *)ptr - sizeof(struct heap_block));
    merge_next(block);
    previous = block->previous;
    if (previous != NULL && previous->free_bin != 0) {
        remove_free(previous);
        /* Insert only the merged result, avoiding unnecessary bin churn. */
        previous->size = previous->size + sizeof(struct heap_block) + block->size;
        previous->next = block->next;
        if (previous->next != NULL) previous->next->previous = previous;
        block = previous;
    }
    insert_free(block);
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
    block = (struct heap_block *)((unsigned char *)ptr - sizeof(struct heap_block));
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
