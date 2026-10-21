/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Guest counterpart of runtime/malloc.c's segregated allocator. Unlike the
 * standalone arena, this heap grows through brk, in small amortized chunks.
 * Sizes are C16 cells; both payloads and headers may cross bank boundaries. */
#include "../mmu-profile.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct block {
    size_t size;
    struct block *next, *previous;
    struct block *free_next, *free_previous;
    unsigned free_bin, magic;
};
#define HEADER ((size_t)sizeof(struct block))
/* Exact small sizes, then doubling bounds covering the complete arena even
 * when all free blocks coalesce. Native's sparse multi-GiB arena needs extra bins. */
#define SMALL_BINS 32
#ifdef FUSE_X_USER
#include "../../../../fuse-x/include/memory-layout.h"
#define BIN_COUNT 58
#define MAX_REQUEST (FUSE_HEAP_CELLS - HEADER)
#else
#define BIN_COUNT 48
#define MAX_REQUEST (16UL * 64000UL - HEADER)
#endif
#define GROW_CELLS 4096UL
#define BLOCK_MAGIC 0xc16bU
static struct block *free_bins[BIN_COUNT], *last;
static unsigned first_free = BIN_COUNT;
static unsigned long heap_start, heap_end;

static unsigned size_bin(size_t size) {
    unsigned index = SMALL_BINS;
    size_t limit = SMALL_BINS * 2;
    if (size <= SMALL_BINS) return (unsigned)size - 1;
    while (size > limit) { limit += limit; index++; }
    return index;
}
static void *data(struct block *b) { return (char *)b + HEADER; }
static void remove_free(struct block *b) {
    unsigned index = b->free_bin - 1;
    struct block *next = b->free_next, *previous = b->free_previous;
    if (previous) previous->free_next = next;
    else free_bins[index] = next;
    if (next) next->free_previous = previous;
    b->free_bin = 0;
}
static void insert_free(struct block *b) {
    unsigned index = size_bin(b->size);
    struct block *next = free_bins[index];
    b->free_bin = index + 1;
    b->free_next = next; b->free_previous = NULL;
    if (next) next->free_previous = b;
    free_bins[index] = b;
    if (index < first_free) first_free = index;
}
/* Physical links also cover gaps created by direct sbrk callers. Only adjacent
 * blocks can coalesce; never absorb memory owned by a different allocator. */
static int adjacent(struct block *b, struct block *next) {
    return (char *)data(b) + b->size == (char *)next;
}
static void merge_next(struct block *b) {
    struct block *next = b->next;
    if (next && next->free_bin && adjacent(b, next)) {
        remove_free(next);
        b->size += HEADER + next->size;
        b->next = next->next;
        if (b->next) b->next->previous = b; else last = b;
        next->magic = 0;
    }
}
static void split_block(struct block *b, size_t size) {
    struct block *tail;
    size_t remaining = b->size - size;
    if (remaining <= HEADER) return;
    tail = (struct block *)((char *)data(b) + size);
    tail->size = remaining - HEADER;
    tail->next = b->next; tail->previous = b;
    tail->free_bin = 0; tail->magic = BLOCK_MAGIC;
    if (tail->next) tail->next->previous = tail; else last = tail;
    b->size = size; b->next = tail;
    merge_next(tail);
    insert_free(tail);
}
/* Direct header recovery replaces the old walk through every live allocation.
 * Range/tag checks retain inexpensive diagnostics for bad and repeated frees;
 * arbitrary invalid pointers remain undefined, as with other C allocators. */
static struct block *allocated(void *p) {
    struct block *b;
    if (!heap_start || (unsigned long)p < (unsigned long)((char *)heap_start + HEADER) ||
        (unsigned long)p >= heap_end) { errno = EINVAL; return NULL; }
    b = (struct block *)((char *)p - HEADER);
    if (b->magic != BLOCK_MAGIC || b->free_bin) { errno = EINVAL; return NULL; }
    return b;
}
static int grow(size_t size) {
    char *current = sbrk(0);
    struct block *b;
    size_t need = size + HEADER, amount;
    int extend;
    if ((unsigned long)current == 0xffffffffUL) return 0;
#ifdef FUSE_X_USER
    /* Preserve compatibility with old packages: only this allocator opts into
     * the larger native arena, before owning any blocks. Direct sbrk storage
     * in the old window remains addressable and is never merged into ours. */
    if (!heap_start && (unsigned long)current < FUSE_HEAP_BASE) {
        if (brk((void *)FUSE_HEAP_BASE) < 0) return 0;
        current = (void *)FUSE_HEAP_BASE;
    }
#endif
    extend = last && last->free_bin && (char *)data(last) + last->size == current;
    if (extend) need = size - last->size;
    amount = need < GROW_CELLS ? GROW_CELLS : need;
#ifdef FUSE_X_USER
    /* Reject exhaustion before forming an out-of-address-space C pointer.
     * At the last banks even a valid request can step past its private range before
     * brk gets a chance to reject it. That is a native runtime fault. */
    {
        unsigned long at=(unsigned long)current;
        size_t available=(FUSE_HEAP_END_BANK-(at>>16))*64000UL-(unsigned)at;
        if(need>available)return 0;
        if(amount>available)amount=need;
    }
#endif
    /* At the heap limit, a smaller final request can still succeed. */
    if (brk(current + amount) < 0) {
        if (amount == need) return 0;
        amount = need;
        if (brk(current + amount) < 0) return 0;
    }
    heap_end = (unsigned long)(current + amount);
    if (extend) {
        b = last; remove_free(b); b->size += amount;
    } else {
        b = (struct block *)current;
        b->size = amount - HEADER; b->previous = last; b->next = NULL;
        b->magic = BLOCK_MAGIC;
        if (last) last->next = b; else heap_start = (unsigned long)b;
        last = b;
    }
    insert_free(b);
    return 1;
}
void *malloc(size_t size) {
    struct block *b;
    unsigned index, pass;
    if (!size) size = 1;
    if (size > MAX_REQUEST) { errno = ENOMEM; return NULL; }
    for (pass = 0; pass < 2; pass++) {
        index = size_bin(size);
        if (index < first_free) index = first_free;
        while (index < BIN_COUNT) {
            b = free_bins[index];
            while (b) {
                if (b->size >= size) {
                    remove_free(b); split_block(b, size); return data(b);
                }
                b = b->free_next;
            }
            if (index == first_free && !free_bins[index]) first_free++;
            index++;
        }
        if (pass || !grow(size)) break;
    }
    errno = ENOMEM; return NULL;
}
void free(void *p) {
    struct block *b, *previous;
    if (!p) return;
    b = allocated(p); if (!b) return;
    merge_next(b);
    previous = b->previous;
    if (previous && previous->free_bin && adjacent(previous, b)) {
        remove_free(previous);
        previous->size += HEADER + b->size; previous->next = b->next;
        if (previous->next) previous->next->previous = previous; else last = previous;
        b->magic = 0; b = previous;
    }
    insert_free(b);
}
void *calloc(size_t count, size_t size) {
    size_t n;
    void *p;
    if (size && count > ~0UL / size) { errno = ENOMEM; return NULL; }
    n = count * size; p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}
void *realloc(void *p, size_t size) {
    struct block *b;
    void *r;
    size_t old_size;
    if (!p) return malloc(size);
    if (!size) { free(p); return NULL; }
    b = allocated(p); if (!b) return NULL;
    if (size > MAX_REQUEST) { errno = ENOMEM; return NULL; }
    old_size = b->size;
    if (b->size < size) merge_next(b);
    if (b->size >= size) { split_block(b, size); return p; }
    r = malloc(size);
    if (r) { memcpy(r, p, old_size); free(p); }
    return r;
}
