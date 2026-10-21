/* Bootstrap fallback for the existing guest cell API. All accesses stay in
 * checked logical memory; loop checkpoints remain scheduling opportunities. */
#include <stddef.h>
#include <stdint.h>

void *__rcc_cell_copy(void *dest, const void *source, size_t count)
{
    unsigned char *d = dest;
    const unsigned char *s = source;
    if (!count)
        return dest;
    /* RAM transfers retain memmove overlap semantics. Bank-zero transfers
     * stay in forward read/write order, as with the existing device API. */
    if (((uintptr_t)d >> 16) && ((uintptr_t)s >> 16) && (uintptr_t)d > (uintptr_t)s) {
        d += count;
        s += count;
        while (count--)
            *--d = *--s;
    } else {
        while (count--)
            *d++ = *s++;
    }
    return dest;
}

void *__rcc_cell_fill(void *dest, int cell, size_t count)
{
    unsigned char *d = dest;
    while (count--)
        *d++ = cell;
    return dest;
}
