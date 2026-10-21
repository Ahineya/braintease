/* Host-only allocation budget for compiler resource regressions. Compile the
 * tool with -Dmalloc=budget_malloc (and calloc/realloc/free), not this file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

union header {
    long double alignment;
    void *pointer;
    size_t size;
};

static size_t live, peak, limit, max_request;
static int ready;

static void report(void)
{
    fprintf(stderr, "HEAP peak=%zu live=%zu limit=%zu\n", peak, live, limit);
}

void *budget_malloc(size_t size)
{
    union header *p;
    if (!ready) {
        const char *s = getenv("RCC_TEST_HEAP_LIMIT");
        limit = s ? (size_t)strtoull(s, NULL, 10) : SIZE_MAX;
        s = getenv("RCC_TEST_MAX_REQUEST");
        max_request = s ? (size_t)strtoull(s, NULL, 10) : SIZE_MAX;
        ready = 1;
        atexit(report);
    }
    if (size > max_request || size > limit - live || size > SIZE_MAX - sizeof *p)
        return NULL;
    p = malloc(sizeof *p + size);
    if (!p)
        return NULL;
    p->size = size;
    live += size;
    if (live > peak)
        peak = live;
    return p + 1;
}

void budget_free(void *pointer)
{
    if (pointer) {
        union header *p = (union header *)pointer - 1;
        live -= p->size;
        free(p);
    }
}

void *budget_calloc(size_t count, size_t size)
{
    void *p;
    if (size && count > SIZE_MAX / size)
        return NULL;
    p = budget_malloc(count * size);
    if (p)
        memset(p, 0, count * size);
    return p;
}

void *budget_realloc(void *pointer, size_t size)
{
    union header *old, *p;
    size_t previous;
    if (!pointer)
        return budget_malloc(size);
    if (!size) {
        budget_free(pointer);
        return NULL;
    }
    old = (union header *)pointer - 1;
    previous = old->size;
    if (size > max_request || size > limit - (live - previous) || size > SIZE_MAX - sizeof *p)
        return NULL;
    p = realloc(old, sizeof *p + size);
    if (!p)
        return NULL;
    p->size = size;
    live = live - previous + size;
    if (live > peak)
        peak = live;
    return p + 1;
}
