#include "process_memory.h"

#define LIVE 0x504d3031U
_Static_assert(sizeof(uint16_t) == 2, "model requires native 16-bit cells");
_Static_assert(X64_PM_PRIVATE_CELLS / X64_PM_PAGE_CELLS == X64_PM_PRIVATE_PAGES,
               "private boundary must be hardware-page aligned");
_Static_assert(X64_PM_TOTAL_BANKS * X64_PM_BANK_CELLS / X64_PM_PAGE_CELLS == X64_PM_TOTAL_PAGES,
               "total boundary must be hardware-page aligned");

static void metrics_clear(X64PmMetrics *m) {
    if (m) *m = (X64PmMetrics){0};
}
static X64PmError ready(const X64PmSpace *s) {
    if (!s) return X64_PM_ARGUMENT;
    if (s->state != LIVE) return X64_PM_STATE;
    return s->busy ? X64_PM_BUSY : X64_PM_OK;
}
static X64PmError span(const X64PmSpace *s, uint32_t bank, uint32_t offset,
                       uint64_t cells, uint64_t *first) {
    X64PmError e = ready(s);
    if (e) return e;
    /* Validate before multiplication; do not treat encoded bank:offset as VA. */
    if (bank >= X64_PM_PRIVATE_BANKS || offset >= X64_PM_BANK_CELLS) return X64_PM_RANGE;
    *first = (uint64_t)bank * X64_PM_BANK_CELLS + offset;
    if (cells > X64_PM_PRIVATE_CELLS - *first) return X64_PM_RANGE;
    if (cells && *first < X64_PM_DEVICE_CELLS) return X64_PM_DEVICE;
    return X64_PM_OK;
}
static unsigned staged(const X64PmSpace *s, unsigned p) {
    return (s->staged[p / 8U] >> (p % 8U)) & 1U;
}
static void stage(X64PmSpace *s, unsigned p, unsigned value) {
    uint8_t bit = (uint8_t)(1U << (p % 8U));
    if (value) s->staged[p / 8U] |= bit;
    else s->staged[p / 8U] &= (uint8_t)~bit;
}
static X64PmError allocate(X64PmSpace *s, unsigned p, X64PmMetrics *m) {
    X64PmBacking b = {0};
    if (!s->ops.alloc4k(s->ops.opaque, &b)) return X64_PM_NOMEM;
    if (m) ++m->allocated_pages;
    /* A successful backend call transfers ownership, even when its returned
     * shape violates the contract. Return that allocation via its callback. */
    if (!b.id || b.id % X64_PM_PAGE_BYTES || !b.bytes ||
            (uintptr_t)b.bytes % X64_PM_PAGE_BYTES) {
        s->ops.free4k(s->ops.opaque, b);
        if (m) ++m->freed_pages;
        return X64_PM_BACKING;
    }
    s->frames[p] = b;
    ++s->allocated;
    stage(s, p, 1);
    return X64_PM_OK;
}
static void free_frame(X64PmSpace *s, unsigned p, X64PmMetrics *m) {
    s->ops.free4k(s->ops.opaque, s->frames[p]);
    s->frames[p] = (X64PmBacking){0};
    --s->allocated;
    if (m) ++m->freed_pages;
}
static void finish_staging(X64PmSpace *s, unsigned first, unsigned end,
                           unsigned rollback, X64PmMetrics *m) {
    for (unsigned p = first; p < end; ++p) {
        if (m) ++m->entries_examined;
        if (!staged(s, p)) continue;
        if (rollback) free_frame(s, p, m);
        stage(s, p, 0);
    }
}
static void copy_bytes(uint8_t *out, const uint8_t *in, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) out[i] = in[i];
}

X64PmError x64_pm_create(X64PmSpace *s, X64PmOps ops) {
    if (!s || !ops.alloc4k || !ops.free4k) return X64_PM_ARGUMENT;
    /* Fresh metadata must be zeroed by its owner. This is not a reset API. */
    if (s->state) return X64_PM_STATE;
    s->ops = ops;
    s->state = LIVE;
    return X64_PM_OK;
}

X64PmError x64_pm_read(const X64PmSpace *s, uint32_t bank, uint32_t offset,
                      uint16_t *out, uint64_t cells, X64PmMetrics *m) {
    metrics_clear(m);
    uint64_t first;
    X64PmError e = span(s, bank, offset, cells, &first);
    if (e) return e;
    if (cells && !out) return X64_PM_ARGUMENT;
    for (uint64_t n = 0; n < cells; ++n) {
        uint64_t at = first + n;
        const uint8_t *bytes = s->frames[at / X64_PM_PAGE_CELLS].bytes;
        size_t offset_bytes = (size_t)(at % X64_PM_PAGE_CELLS) * 2U;
        out[n] = bytes ? (uint16_t)(bytes[offset_bytes] | (uint16_t)bytes[offset_bytes + 1] << 8) : 0;
        if (bytes && m) m->payload_bytes_read += 2;
    }
    return X64_PM_OK;
}

X64PmError x64_pm_write(X64PmSpace *s, uint32_t bank, uint32_t offset,
                       const uint16_t *in, uint64_t cells, X64PmMetrics *m) {
    metrics_clear(m);
    uint64_t first;
    X64PmError e = span(s, bank, offset, cells, &first);
    if (e) return e;
    if (cells && !in) return X64_PM_ARGUMENT;
    if (!cells) return X64_PM_OK;
    unsigned begin = (unsigned)(first / X64_PM_PAGE_CELLS);
    unsigned end = (unsigned)((first + cells - 1) / X64_PM_PAGE_CELLS) + 1;
    s->busy = 1;
    /* Obtain every missing page before touching any existing payload. */
    for (unsigned p = begin; p < end; ++p) {
        if (m) ++m->entries_examined;
        if (s->frames[p].id) continue;
        e = allocate(s, p, m);
        if (e) {
            finish_staging(s, begin, end, 1, m);
            s->busy = 0;
            return e;
        }
    }
    for (uint64_t n = 0; n < cells; ++n) {
        uint64_t at = first + n;
        uint8_t *bytes = s->frames[at / X64_PM_PAGE_CELLS].bytes;
        size_t offset_bytes = (size_t)(at % X64_PM_PAGE_CELLS) * 2U;
        bytes[offset_bytes] = (uint8_t)in[n];
        bytes[offset_bytes + 1] = (uint8_t)(in[n] >> 8);
    }
    if (m) m->payload_bytes_written = cells * 2;
    finish_staging(s, begin, end, 0, m);
    s->busy = 0;
    return X64_PM_OK;
}

X64PmError x64_pm_zero(X64PmSpace *s, uint32_t bank, uint32_t offset,
                      uint64_t cells, X64PmMetrics *m) {
    metrics_clear(m);
    uint64_t first;
    X64PmError e = span(s, bank, offset, cells, &first);
    if (e) return e;
    for (uint64_t n = 0; n < cells; ++n) {
        uint64_t at = first + n;
        uint8_t *bytes = s->frames[at / X64_PM_PAGE_CELLS].bytes;
        if (!bytes) continue;
        size_t offset_bytes = (size_t)(at % X64_PM_PAGE_CELLS) * 2U;
        bytes[offset_bytes] = bytes[offset_bytes + 1] = 0;
        if (m) m->payload_bytes_written += 2;
    }
    /* Zero does not allocate holes or change identities of existing frames. */
    return X64_PM_OK;
}

X64PmError x64_pm_clone(X64PmSpace *dst, X64PmSpace *src, X64PmMetrics *m) {
    metrics_clear(m);
    X64PmError e = ready(dst);
    if (e) return e;
    e = ready(src);
    if (e) return e;
    if (dst == src || dst->allocated) return X64_PM_STATE;
    dst->busy = src->busy = 1;
    for (unsigned p = 0; p < X64_PM_PRIVATE_PAGES; ++p) {
        if (m) ++m->entries_examined;
        if (!src->frames[p].id) continue;
        e = allocate(dst, p, m);
        if (e) {
            finish_staging(dst, 0, X64_PM_PRIVATE_PAGES, 1, m);
            dst->busy = src->busy = 0;
            return e;
        }
    }
    for (unsigned p = 0; p < X64_PM_PRIVATE_PAGES; ++p) {
        if (m) ++m->entries_examined;
        if (!src->frames[p].id) continue;
        copy_bytes(dst->frames[p].bytes, src->frames[p].bytes, X64_PM_PAGE_BYTES);
        if (m) m->payload_bytes_copied += X64_PM_PAGE_BYTES;
    }
    finish_staging(dst, 0, X64_PM_PRIVATE_PAGES, 0, m);
    dst->busy = src->busy = 0;
    return X64_PM_OK;
}

X64PmError x64_pm_release(X64PmSpace *s, X64PmMetrics *m) {
    metrics_clear(m);
    if (!s) return X64_PM_ARGUMENT;
    if (!s->state) return X64_PM_OK;
    X64PmError e = ready(s);
    if (e) return e;
    s->busy = 1;
    for (unsigned p = 0; p < X64_PM_PRIVATE_PAGES; ++p) {
        if (m) ++m->entries_examined;
        if (s->frames[p].id) free_frame(s, p, m);
    }
    s->ops = (X64PmOps){0};
    s->busy = s->state = 0;
    return X64_PM_OK;
}

X64PmError x64_pm_plan(const X64PmSpace *s, uint32_t first, size_t capacity,
                      X64PmMapping *out, size_t *written, X64PmMetrics *m) {
    metrics_clear(m);
    X64PmError e = ready(s);
    if (e) return e;
    if (!written) return X64_PM_ARGUMENT;
    if (first > X64_PM_TOTAL_PAGES) return X64_PM_RANGE;
    size_t count = X64_PM_TOTAL_PAGES - first;
    if (count > capacity) count = capacity;
    if (count && !out) return X64_PM_ARGUMENT;
    for (size_t n = 0; n < count; ++n) {
        unsigned p = first + (unsigned)n;
        X64PmMapping entry = {.page = p, .contains_device_cells = p == 0};
        if (p >= X64_PM_PRIVATE_PAGES) entry.region = X64_PM_PLATFORM_SHARED;
        else {
            entry.backing_id = s->frames[p].id;
            entry.region = entry.backing_id ? X64_PM_PRIVATE_BACKED : X64_PM_PRIVATE_ZERO;
        }
        out[n] = entry;
    }
    *written = count;
    if (m) m->entries_examined = count;
    return X64_PM_OK;
}
