#ifndef DOOM_ALLOC_FAST_H
#define DOOM_ALLOC_FAST_H

#include <stddef.h>

/* Counts use C storage bytes, which are 16-bit cells on Ripple. */
int Doom_AllocationSize(size_t count, size_t size, size_t *bytes);
void *Doom_AllocZeroed(size_t bytes);
void *Doom_Calloc(size_t count, size_t size);

#endif
