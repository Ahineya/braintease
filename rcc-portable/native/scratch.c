/* Generated operand strings and function analysis have function lifetime.
 * They must not enter the parser's module-lifetime intern table/IR arena. */
#include "native.h"
#include <stdlib.h>
#include <string.h>

union scratch_cell {
    void *pointer;
    unsigned long long bits;
    double number;
};

struct scratch_chunk {
    struct scratch_chunk *next;
    size_t used, capacity;
    union scratch_cell data[1];
};
static struct scratch_chunk *chunks;

void *native_scratch(size_t bytes)
{
    size_t count = bytes / sizeof(union scratch_cell) + (bytes % sizeof(union scratch_cell) != 0);
    void *result;
    if (!count)
        count = 1;
    if (!chunks || count > chunks->capacity - chunks->used) {
        size_t capacity = count > 4096 ? count : 4096;
        struct scratch_chunk *chunk;
        if (capacity >
            ((size_t)-1 - offsetof(struct scratch_chunk, data)) / sizeof(union scratch_cell))
            fail("native scratch allocation overflow");
        chunk =
            calloc(1, offsetof(struct scratch_chunk, data) + capacity * sizeof(union scratch_cell));
        if (!chunk)
            fail("out of memory allocating native function scratch");
        chunk->capacity = capacity;
        chunk->next = chunks;
        chunks = chunk;
    }
    result = chunks->data + chunks->used;
    chunks->used += count;
    return result;
}

char *native_copy(const char *text)
{
    size_t size = strlen(text) + 1;
    char *copy = native_scratch(size);
    memcpy(copy, text, size);
    return copy;
}

void native_scratch_reset(void)
{
    while (chunks) {
        struct scratch_chunk *next = chunks->next;
        free(chunks);
        chunks = next;
    }
}
