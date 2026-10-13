#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "alloc_fast.h"

int Doom_AllocationSize(size_t count, size_t size, size_t *bytes)
{
#ifdef RIPPLE
    size_t low_product;
    size_t high_product;
    unsigned int count_high;
    unsigned int size_high;

    if (count == 0 || size == 0) {
        *bytes = 0;
        return 1;
    }
    if (size == 1) {
        *bytes = count;
        return 1;
    }
    if (count == 1) {
        *bytes = size;
        return 1;
    }

    /* size_t has two 16-bit words. Avoid the generic 32-bit division in
     * SIZE_MAX / count, including when the count crosses a memory bank.
     * Shifts by 16 just select/place words in the Ripple calling model. */
    count_high = (unsigned int)(count >> 16);
    size_high = (unsigned int)(size >> 16);
    if (count_high != 0 && size_high != 0)
        return 0;

    low_product = (size_t)(unsigned int)count * (unsigned int)size;
    if (count_high == 0 && size_high == 0) {
        *bytes = low_product;
        return 1;
    }
    if (count_high != 0)
        high_product = (size_t)count_high * (unsigned int)size;
    else
        high_product = (size_t)size_high * (unsigned int)count;

    /* Both partial products fit in 32 bits. This addition cannot overflow:
     * 65535*65535 + 65534 is still below SIZE_MAX. */
    if (high_product + (low_product >> 16) > 65535UL)
        return 0;
    *bytes = low_product + (high_product << 16);
#else
    if (count != 0 && size > SIZE_MAX / count)
        return 0;
    *bytes = count * size;
#endif
    return 1;
}

void *Doom_AllocZeroed(size_t bytes)
{
    void *result;
    if (bytes == 0)
        return NULL;
    result = malloc(bytes);
    if (result != NULL)
        memset(result, 0, bytes);
    return result;
}

void *Doom_Calloc(size_t count, size_t size)
{
    size_t bytes;
    if (!Doom_AllocationSize(count, size, &bytes))
        return NULL;
    return Doom_AllocZeroed(bytes);
}
