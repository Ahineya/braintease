/* Function-lifetime QBE storage. Fresh zeroed slabs preserve alloc()'s contract;
 * no address moves, and freeall() ends precisely the original pool lifetime.
 * Large requests get their own slab without displacing the active small slab. */
typedef union {
    long long integer;
    double floating;
    void *pointer;
} QbeAlignment;

typedef struct QbeSlab QbeSlab;

struct QbeSlab {
    QbeSlab *next;
    size_t used, capacity;
    QbeAlignment alignment;
};

static QbeSlab *qbe_slabs, *qbe_active_slab;

void *alloc(size_t size)
{
    const size_t chunk = 16384, alignment = sizeof(QbeAlignment);
    QbeSlab *slab;
    void *result;
    if (!size)
        return 0;
    if (size > (size_t)-1 - sizeof(QbeSlab) - alignment)
        die("allocation span overflow");
    size = (size + alignment - 1) / alignment * alignment;
    if (size > chunk || !qbe_active_slab ||
        size > qbe_active_slab->capacity - qbe_active_slab->used) {
        size_t capacity = size > chunk ? size : chunk;
        slab = emalloc(sizeof(QbeSlab) + capacity);
        slab->next = qbe_slabs;
        slab->capacity = capacity;
        qbe_slabs = slab;
        if (size <= chunk)
            qbe_active_slab = slab;
    } else
        slab = qbe_active_slab;
    result = (char *)(slab + 1) + slab->used;
    slab->used += size;
    return result;
}

void freeall(void)
{
    QbeSlab *slab;
    while (qbe_slabs) {
        slab = qbe_slabs;
        qbe_slabs = slab->next;
        free(slab);
    }
    qbe_active_slab = 0;
}
