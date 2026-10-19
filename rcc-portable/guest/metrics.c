/* Optional accounting for allocations made by the compiler, in C16 cells.
 * SDK allocations and allocator metadata are outside these counters. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef malloc
#undef calloc
#undef realloc
#undef free
void *malloc(size_t);
void free(void *);

static size_t live, peak;
static unsigned long requests;
static int registered;

static void report(void)
{
    fprintf(stderr, "rcc-c: heap peak %lu cells; allocations %lu\n", peak, requests);
}

void *rcc_c_malloc(size_t n)
{
    size_t *block;
    if (n > ~0UL - sizeof(size_t)) {
        errno = ENOMEM;
        return NULL;
    }
    if (!registered) {
        registered = 1;
        atexit(report);
    }
    block = malloc(n + sizeof(size_t));
    if (!block)
        return NULL;
    *block = n;
    live += n;
    if (live > peak)
        peak = live;
    requests++;
    return block + 1;
}

void rcc_c_free(void *p)
{
    size_t *block;
    if (!p)
        return;
    block = (size_t *)p - 1;
    live -= *block;
    free(block);
}

void *rcc_c_calloc(size_t n, size_t size)
{
    void *p;
    if (size && n > ~0UL / size) {
        errno = ENOMEM;
        return NULL;
    }
    n *= size;
    p = rcc_c_malloc(n);
    if (p)
        memset(p, 0, n);
    return p;
}

void *rcc_c_realloc(void *p, size_t n)
{
    void *next;
    size_t previous;
    if (!p)
        return rcc_c_malloc(n);
    if (!n) {
        rcc_c_free(p);
        return NULL;
    }
    previous = *((size_t *)p - 1);
    next = rcc_c_malloc(n);
    if (next) {
        memcpy(next, p, previous < n ? previous : n);
        rcc_c_free(p);
    }
    return next;
}
