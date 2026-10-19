/* Freestanding C16 allocator. Eight-cell block alignment keeps payload offsets
 * nonzero across 64000-cell banks, matching RFL's compatibility null checks. */
typedef unsigned long size_t;
typedef unsigned short cell;
extern cell *__wasm_heap_base;
extern unsigned long __wasm_heap_cells;
extern void __wasm_trap(int code);
struct block { size_t cells; struct block *next; unsigned magic; cell pad[3]; };
static struct block *head;
static int initialized;
/* Successful allocations, live blocks, reserved payload cells. Read-only host
 * telemetry uses one exported address, not a service crossing per allocation. */
#ifdef RIPPLE_WASM_BROWSER
static unsigned long metrics[3];
cell *__wasm_heap_metrics(void) { return (cell *)metrics; }
#endif

void *malloc(size_t count) {
    struct block *b, *previous = 0, *tail;
    size_t needed;
    if (!count || count > 16384000UL - 16UL) return 0;
    needed = ((count + 7UL) & ~7UL) + 8UL;
    if (!initialized) {
        initialized = 1;
        head = (struct block *)__wasm_heap_base;
        head->cells = __wasm_heap_cells & ~7UL;
        head->next = 0; head->magic = 0xfeee;
    }
    b = head;
    while (b) {
        if (b->magic != 0xfeee) __wasm_trap(4);
        if (b->cells >= needed) {
            if (b->cells - needed >= 16UL) {
                tail = (struct block *)((cell *)b + needed);
                tail->cells = b->cells - needed; tail->next = b->next; tail->magic = 0xfeee;
                b->cells = needed;
            } else tail = b->next;
            if (previous) previous->next = tail; else head = tail;
            b->magic = 0xa110; b->next = 0;
#ifdef RIPPLE_WASM_BROWSER
            ++metrics[0]; ++metrics[1]; metrics[2] += b->cells - 8UL;
#endif
            return (cell *)b + 8;
        }
        previous = b; b = b->next;
    }
    return 0;
}
void free(void *pointer) {
    struct block *b, *previous = 0, *next;
    if (!pointer) return;
    if ((cell *)pointer < __wasm_heap_base + 8) __wasm_trap(4);
    b = (struct block *)((cell *)pointer - 8);
    if (b->magic != 0xa110 || b->cells < 8UL) __wasm_trap(4);
#ifdef RIPPLE_WASM_BROWSER
    --metrics[1]; metrics[2] -= b->cells - 8UL;
#endif
    b->magic = 0xfeee;
    next = head;
    while (next && next < b) { previous = next; next = next->next; }
    b->next = next;
    if (next && (cell *)b + b->cells == (cell *)next) {
        b->cells += next->cells; b->next = next->next;
    }
    if (previous) {
        previous->next = b;
        if ((cell *)previous + previous->cells == (cell *)b) {
            previous->cells += b->cells; previous->next = b->next;
        }
    } else head = b;
}
void *memset(void *pointer, int value, size_t count) {
    cell *p = pointer; size_t i;
    for (i = 0; i < count; ++i) p[i] = value;
    return pointer;
}
void *memcpy(void *destination, const void *source, size_t count) {
    cell *d = destination; const cell *s = source; size_t i;
    for (i = 0; i < count; ++i) d[i] = s[i];
    return destination;
}
void *memmove(void *destination, const void *source, size_t count) {
    cell *d = destination; const cell *s = source; size_t i;
    if (d < s) return memcpy(destination, source, count);
    i = count; while (i) { --i; d[i] = s[i]; }
    return destination;
}
void *calloc(size_t count, size_t size) {
    void *p;
    if (size && count > 16384000UL / size) return 0;
    p = malloc(count * size);
    if (p) memset(p, 0, count * size);
    return p;
}
void *realloc(void *pointer, size_t count) {
    void *p; size_t old;
    if (!pointer) return malloc(count);
    if (!count) { free(pointer); return 0; }
    old = ((struct block *)((cell *)pointer - 8))->cells - 8;
    p = malloc(count);
    if (!p) return 0;
    memcpy(p, pointer, count < old ? count : old);
    free(pointer); return p;
}
