/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include "qjs-platform.h"

typedef union QJSAllocationHeader {
    struct { size_t size; uint32_t magic; void *raw; } data;
    double number_alignment;
    void *pointer_alignment;
} QJSAllocationHeader;

#define QJS_ALLOCATION_MAGIC 0x716a734dUL
#define QJS_ALLOCATION_ALIGNMENT 4U
#define QJS_ALLOCATION_OVERHEAD (sizeof(QJSAllocationHeader) + QJS_ALLOCATION_ALIGNMENT - 1)

size_t qjs_c16_allocation_overhead(void)
{
    return QJS_ALLOCATION_OVERHEAD;
}

/* Bank sizes are multiples of four, so the low pointer bits also describe
 * alignment across a bank boundary. Do arithmetic on pointers, not packed
 * (bank, offset) integers. The provider may return any cell alignment.
 */
static void *aligned_payload(void *raw)
{
    unsigned char *p = (unsigned char *)raw + sizeof(QJSAllocationHeader);
    size_t padding = (QJS_ALLOCATION_ALIGNMENT - ((uintptr_t)p & 3U)) & 3U;
    return p + padding;
}

uint64_t qjs_c16_physical_octets(size_t cells)
{
    return (uint64_t)cells * 2;
}

static QJSAllocationHeader *allocation_header(const void *ptr)
{
    QJSAllocationHeader *h = (QJSAllocationHeader *)ptr - 1;
    assert(h->data.magic == QJS_ALLOCATION_MAGIC);
    return h;
}

static size_t allocation_size(const void *ptr)
{
    return ptr ? allocation_header(ptr)->data.size : 0;
}

static long allocation_allowed(JSMallocState *s, size_t size, size_t old_charge)
{
    size_t charge, base;
    QJSAllocatorControl *control = s->opaque;
    if (!size || size > SIZE_MAX - QJS_ALLOCATION_OVERHEAD) return 0;
    charge = size + QJS_ALLOCATION_OVERHEAD;
    assert(old_charge <= s->malloc_size);
    base = s->malloc_size - old_charge;
    if (charge > SIZE_MAX - base) return 0;
    /* Permit shrinking even after a limit was lowered below current usage. */
    if (charge > old_charge &&
        (base > s->malloc_limit || charge > s->malloc_limit - base)) return 0;
    if (control) {
        ++control->calls;
        if (control->fail_at && control->calls == control->fail_at) return 0;
    }
    return 1;
}

static void record_peak(JSMallocState *s)
{
    QJSAllocatorControl *control = s->opaque;
    if (control && control->peak_cells < s->malloc_size)
        control->peak_cells = s->malloc_size;
}

static void *allocate(JSMallocState *s, size_t size)
{
    QJSAllocationHeader *h;
    void *raw, *payload;
    if (s->malloc_count == SIZE_MAX || !allocation_allowed(s, size, 0)) return NULL;
    raw = malloc(QJS_ALLOCATION_OVERHEAD + size);
    if (!raw) return NULL;
    payload = aligned_payload(raw);
    h = (QJSAllocationHeader *)payload - 1;
    h->data.size = size;
    h->data.magic = QJS_ALLOCATION_MAGIC;
    h->data.raw = raw;
    ++s->malloc_count;
    s->malloc_size += QJS_ALLOCATION_OVERHEAD + size;
    record_peak(s);
    return payload;
}

static void release(JSMallocState *s, void *ptr)
{
    QJSAllocationHeader *h;
    size_t charge;
    if (!ptr) return;
    h = allocation_header(ptr);
    charge = QJS_ALLOCATION_OVERHEAD + h->data.size;
    assert(s->malloc_count && charge <= s->malloc_size);
    --s->malloc_count;
    s->malloc_size -= charge;
    h->data.magic = 0;
    free(h->data.raw);
}

static void *resize(JSMallocState *s, void *ptr, size_t size)
{
    QJSAllocationHeader *h, *fresh;
    size_t old_charge, old_size, old_offset;
    void *raw, *payload;
    if (!size) { release(s, ptr); return NULL; }
    if (!ptr) return allocate(s, size);
    h = allocation_header(ptr);
    old_size = h->data.size;
    old_charge = QJS_ALLOCATION_OVERHEAD + old_size;
    old_offset = (unsigned char *)ptr - (unsigned char *)h->data.raw;
    if (!allocation_allowed(s, size, old_charge)) return NULL;
    raw = realloc(h->data.raw, QJS_ALLOCATION_OVERHEAD + size);
    if (!raw) return NULL;
    payload = aligned_payload(raw);
    /* realloc can change alignment. Move the copied prefix before writing
     * the new header, which may overlap the old payload/header. */
    memmove(payload, (unsigned char *)raw + old_offset, old_size < size ? old_size : size);
    fresh = (QJSAllocationHeader *)payload - 1;
    fresh->data.size = size;
    fresh->data.magic = QJS_ALLOCATION_MAGIC;
    fresh->data.raw = raw;
    s->malloc_size = s->malloc_size - old_charge + QJS_ALLOCATION_OVERHEAD + size;
    record_peak(s);
    return payload;
}

const JSMallocFunctions qjs_c16_malloc_functions = {
    allocate, release, resize, allocation_size
};

uintptr_t qjs_c16_linear_address(const void *pointer)
{
    uintptr_t raw = (uintptr_t)pointer;
    return (raw >> 16) * 64000UL + (raw & 65535UL);
}

long qjs_c16_stack_at(uintptr_t base, size_t limit, size_t requested,
                      uintptr_t here)
{
    size_t distance;
    if (!limit) return 0;
    distance = here < base ? base - here : here - base;
    return distance >= limit || requested > limit - distance;
}

long qjs_c16_check_stack(uintptr_t base, size_t limit, size_t requested)
{
    volatile unsigned marker = 0;
    return qjs_c16_stack_at(base, limit, requested,
                            qjs_c16_linear_address((const void *)&marker));
}
