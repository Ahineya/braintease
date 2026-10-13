#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "alloc_fast.h"

static int product(size_t count, size_t size, size_t expected)
{
    size_t result = 123;
    return Doom_AllocationSize(count, size, &result) && result == expected;
}

static int overflow(size_t count, size_t size)
{
    size_t result = 123;
    return !Doom_AllocationSize(count, size, &result) && result == 123;
}

static int cells(const unsigned char *data, size_t count, unsigned int value)
{
    while (count != 0) {
        if (*data != value)
            return 0;
        ++data;
        --count;
    }
    return 1;
}

int main(void)
{
    unsigned char *left;
    unsigned char *dirty;
    unsigned char *right;
    unsigned char *zeroed;
    size_t count = 65540UL;
    size_t bytes = count * 2;

    if (!product(0, SIZE_MAX, 0) || !product(SIZE_MAX, 0, 0)
        || !product(1, SIZE_MAX, SIZE_MAX) || !product(SIZE_MAX, 1, SIZE_MAX)
        || !product(65535UL, 65535UL, 4294836225UL)
        || !product(65537UL, 65535UL, 4294967295UL)
        || !product(65535UL, 65537UL, 4294967295UL)
        || !product(count, 2, bytes) || !product(2, count, bytes))
        return 1;
    if (!overflow(SIZE_MAX, 2) || !overflow(2, SIZE_MAX)
        || !overflow(SIZE_MAX / 2 + 1, 2))
        return 2;
#ifdef RIPPLE
    if (!overflow(65536UL, 65536UL)
        || !overflow(65538UL, 65535UL)
        || !overflow(65535UL, 65538UL))
        return 3;
    /* Exercise the public calloc redirection as well as the helper API. */
    if (calloc(0, 1) != NULL)
        return 4;
#endif
    if (Doom_Calloc(0, 1) != NULL || Doom_Calloc(1, 0) != NULL
        || Doom_Calloc(0, 0) != NULL || Doom_Calloc(SIZE_MAX, 2) != NULL
        || Doom_Calloc(SIZE_MAX, 1) != NULL || Doom_AllocZeroed(0) != NULL)
        return 5;

    left = malloc(23);
    dirty = malloc(bytes);
    right = malloc(31);
    if (left == NULL || dirty == NULL || right == NULL)
        return 6;
    memset(left, 91, 23);
    memset(right, 173, 31);
    memset(dirty, UCHAR_MAX, bytes);
    if (!cells(dirty, bytes, UCHAR_MAX))
        return 7;
    free(dirty);
#ifdef RIPPLE
    zeroed = calloc(count, 2);
#else
    zeroed = Doom_Calloc(count, 2);
#endif
    if (zeroed == NULL)
        return 8;
#ifdef RIPPLE
    /* The first-fit runtime must reuse our dirty block. This proves the
     * clearing test is not accidentally reading untouched initial tape. */
    if (zeroed != dirty)
        return 9;
#endif
    if (!cells(zeroed, bytes, 0) || !cells(left, 23, 91)
        || !cells(right, 31, 173))
        return 10;
    free(zeroed);
    free(left);
    free(right);
    puts("ALLOC FAST OK zero sizes, overflow, reused cells, wide products and bank crossings");
    return 0;
}
