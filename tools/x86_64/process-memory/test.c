#include "process_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define OK(c) REQUIRE((c) == X64_PM_OK)
#define POOL_SLOTS (2U * X64_PM_PRIVATE_PAGES + 8U)
typedef struct { X64PmBacking advertised; void *allocation; } Slot;
typedef struct {
    Slot slots[POOL_SLOTS];
    uint64_t attempts, allocated, freed, fail_at;
    unsigned live, next_slot, bad_shape, dirty_failure;
    X64PmSpace *probe[2];
    unsigned busy_checks;
} Pool;
static unsigned groups;
static uint64_t dense_clone_copied, dense_plan_entries;

static void busy_probe(Pool *p) {
    for (unsigned n = 0; n < 2; ++n) if (p->probe[n]) {
        uint16_t value = 0;
        size_t written = 19;
        REQUIRE(x64_pm_read(p->probe[n], 0, 32, &value, 1, NULL) == X64_PM_BUSY);
        REQUIRE(x64_pm_release(p->probe[n], NULL) == X64_PM_BUSY);
        REQUIRE(x64_pm_plan(p->probe[n], 0, 0, NULL, &written, NULL) == X64_PM_BUSY);
        REQUIRE(written == 19);
        ++p->busy_checks;
    }
}
static int alloc4k(void *opaque, X64PmBacking *out) {
    Pool *p = opaque;
    busy_probe(p);
    if (++p->attempts == p->fail_at) {
        if (p->dirty_failure) *out = (X64PmBacking){4096, (void *)(uintptr_t)4096};
        return 0;
    }
    unsigned slot = p->next_slot;
    while (p->slots[slot].allocation) {
        slot = (slot + 1) % POOL_SLOTS;
        REQUIRE(slot != p->next_slot);
    }
    uint8_t *bytes = aligned_alloc(X64_PM_PAGE_BYTES, X64_PM_PAGE_BYTES);
    REQUIRE(bytes && (uintptr_t)bytes % X64_PM_PAGE_BYTES == 0);
    memset(bytes, 0, X64_PM_PAGE_BYTES);
    X64PmBacking b = {(uint64_t)(slot + 1) * X64_PM_PAGE_BYTES, bytes};
    REQUIRE(b.id != (uintptr_t)b.bytes);
    switch (p->bad_shape) {
    case 1: b.id = 0; break;
    case 2: ++b.id; break;
    case 3: b.bytes = NULL; break;
    case 4: ++b.bytes; break;
    default: break;
    }
    p->bad_shape = 0;
    p->slots[slot] = (Slot){b, bytes};
    p->next_slot = (slot + 1) % POOL_SLOTS;
    ++p->allocated; ++p->live;
    *out = b;
    return 1;
}
static void free4k(void *opaque, X64PmBacking b) {
    Pool *p = opaque;
    busy_probe(p);
    unsigned slot = b.id && b.id % X64_PM_PAGE_BYTES == 0 ?
        (unsigned)(b.id / X64_PM_PAGE_BYTES - 1) : POOL_SLOTS;
    if (slot >= POOL_SLOTS || !p->slots[slot].allocation ||
            p->slots[slot].advertised.bytes != b.bytes) {
        for (slot = 0; slot < POOL_SLOTS; ++slot)
            if (p->slots[slot].allocation && p->slots[slot].advertised.id == b.id &&
                    p->slots[slot].advertised.bytes == b.bytes) break;
    }
    REQUIRE(slot < POOL_SLOTS && p->slots[slot].allocation);
    REQUIRE(p->slots[slot].advertised.id == b.id && p->live);
    memset(p->slots[slot].allocation, 0xdd, X64_PM_PAGE_BYTES);
    free(p->slots[slot].allocation);
    p->slots[slot] = (Slot){0};
    ++p->freed; --p->live;
}
static Pool *pool(void) {
    Pool *p = calloc(1, sizeof(*p));
    REQUIRE(p);
    return p;
}
static X64PmSpace *space(Pool *p) {
    X64PmSpace *s = calloc(1, sizeof(*s));
    REQUIRE(s);
    OK(x64_pm_create(s, (X64PmOps){p, alloc4k, free4k}));
    return s;
}
static void destroy(X64PmSpace *s) {
    OK(x64_pm_release(s, NULL));
    OK(x64_pm_release(s, NULL));
    free(s);
}
static void finish(Pool *p) {
    REQUIRE(p->live == 0 && p->allocated == p->freed);
    for (unsigned n = 0; n < POOL_SLOTS; ++n) REQUIRE(!p->slots[n].allocation);
    free(p);
    ++groups;
}
static void write_at(X64PmSpace *s, uint32_t at, const uint16_t *data, uint64_t n) {
    OK(x64_pm_write(s, at / X64_PM_BANK_CELLS, at % X64_PM_BANK_CELLS, data, n, NULL));
}
static void read_at(const X64PmSpace *s, uint32_t at, uint16_t *data, uint64_t n) {
    OK(x64_pm_read(s, at / X64_PM_BANK_CELLS, at % X64_PM_BANK_CELLS, data, n, NULL));
}
static uint64_t identity(const X64PmSpace *s, unsigned page) {
    X64PmMapping m;
    size_t n;
    OK(x64_pm_plan(s, page, 1, &m, &n, NULL));
    REQUIRE(n == 1);
    return m.backing_id;
}

static void empty_and_plan(void) {
    Pool *p = pool(); X64PmSpace *s = space(p);
    uint16_t out[3] = {1, 2, 3};
    read_at(s, 32, out, 3);
    REQUIRE(!out[0] && !out[1] && !out[2] && !p->attempts);
    size_t total = 0;
    X64PmMetrics metrics;
    while (total < X64_PM_TOTAL_PAGES) {
        X64PmMapping mappings[31]; size_t count;
        OK(x64_pm_plan(s, (uint32_t)total, 31, mappings, &count, &metrics));
        REQUIRE(count && count <= 31 && metrics.entries_examined == count);
        REQUIRE(!metrics.payload_bytes_copied && !metrics.allocated_pages);
        for (size_t i = 0; i < count; ++i) {
            X64PmMapping *m = mappings + i;
            REQUIRE(m->page == total + i && !m->backing_id);
            REQUIRE(m->region == (m->page < X64_PM_PRIVATE_PAGES ?
                        X64_PM_PRIVATE_ZERO : X64_PM_PLATFORM_SHARED));
            REQUIRE(m->contains_device_cells == (m->page == 0));
        }
        total += count;
    }
    size_t count = 99;
    OK(x64_pm_plan(s, X64_PM_TOTAL_PAGES, SIZE_MAX, NULL, &count, NULL));
    REQUIRE(count == 0);
    X64PmMapping last;
    OK(x64_pm_plan(s, X64_PM_TOTAL_PAGES - 1, SIZE_MAX, &last, &count, NULL));
    REQUIRE(count == 1 && last.region == X64_PM_PLATFORM_SHARED);
    destroy(s); finish(p);
    puts("PASS sparse zero backing and bounded private/shared mapping plans");
}

static void boundaries(void) {
    Pool *p = pool(); X64PmSpace *s = space(p);
    const uint32_t starts[] = {2047, 63999, 65535, X64_PM_PRIVATE_CELLS - 4};
    const uint16_t values[] = {0xffff, 0x8000, 0x1234, 0xff};
    for (unsigned i = 0; i < sizeof(starts) / sizeof(starts[0]); ++i) {
        uint16_t out[6];
        write_at(s, starts[i], values, 4);
        read_at(s, starts[i], out, 4);
        REQUIRE(!memcmp(out, values, sizeof(values)));
    }
    /* 64000 cells is not page aligned; bank0:63999 and bank1:0 share page31. */
    REQUIRE(identity(s, 31) && identity(s, 32));
    uint16_t out[4]; read_at(s, 63999, out, 4);
    REQUIRE(!memcmp(out, values, sizeof(values)));
    X64PmMapping m; size_t count;
    OK(x64_pm_plan(s, 0, 1, &m, &count, NULL));
    REQUIRE(m.region == X64_PM_PRIVATE_BACKED && m.contains_device_cells);
    destroy(s); finish(p);
    puts("PASS full-width cells, hardware-page/bank crossings and private end");
}

static void rejected_spans(void) {
    Pool *p = pool(); X64PmSpace *s = space(p);
    uint16_t value = 0x1234, out = 0xbeef;
    const uint32_t banks[] = {240, 256, UINT32_MAX, 0, 239, 0};
    const uint32_t offsets[] = {0, 0, 0, 64000, 63999, 32};
    const uint64_t counts[] = {1, 0, 0, 0, 2, UINT64_MAX};
    for (unsigned i = 0; i < sizeof(banks) / sizeof(banks[0]); ++i) {
        REQUIRE(x64_pm_read(s, banks[i], offsets[i], &out, counts[i], NULL) == X64_PM_RANGE);
        REQUIRE(out == 0xbeef);
        REQUIRE(x64_pm_write(s, banks[i], offsets[i], &value, counts[i], NULL) == X64_PM_RANGE);
        REQUIRE(x64_pm_zero(s, banks[i], offsets[i], counts[i], NULL) == X64_PM_RANGE);
    }
    REQUIRE(x64_pm_read(s, 0, 31, &out, 2, NULL) == X64_PM_DEVICE);
    REQUIRE(x64_pm_write(s, 0, 0, &value, 1, NULL) == X64_PM_DEVICE);
    REQUIRE(x64_pm_zero(s, 0, 0, 32, NULL) == X64_PM_DEVICE);
    OK(x64_pm_read(s, 0, 0, NULL, 0, NULL));
    OK(x64_pm_write(s, 0, 0, NULL, 0, NULL));
    OK(x64_pm_zero(s, 0, 0, 0, NULL));
    REQUIRE(x64_pm_read(s, 0, 32, NULL, 1, NULL) == X64_PM_ARGUMENT);
    REQUIRE(x64_pm_write(s, 0, 32, NULL, 1, NULL) == X64_PM_ARGUMENT);
    REQUIRE(x64_pm_read(NULL, 0, 32, &out, 1, NULL) == X64_PM_ARGUMENT);
    X64PmMapping entry = {.backing_id = 123}; size_t count = 91;
    REQUIRE(x64_pm_plan(s, 8001, 1, &entry, &count, NULL) == X64_PM_RANGE);
    REQUIRE(x64_pm_plan(s, 0, 1, NULL, &count, NULL) == X64_PM_ARGUMENT);
    REQUIRE(x64_pm_plan(s, 0, 1, &entry, NULL, NULL) == X64_PM_ARGUMENT);
    REQUIRE(entry.backing_id == 123 && count == 91 && !p->attempts);
    destroy(s); finish(p);
    puts("PASS span overflow, shared/device denial, zero-length and untouched outputs");
}

static void transactional_write(void) {
    Pool *p = pool(); X64PmSpace *s = space(p);
    const uint16_t seed = 0xbad;
    write_at(s, 32, &seed, 1); write_at(s, 8192, &seed, 1);
    uint16_t *before = calloc(9000, sizeof(uint16_t));
    uint16_t *after = calloc(9000, sizeof(uint16_t));
    uint16_t *input = malloc(7000 * sizeof(uint16_t));
    X64PmSpace *metadata = malloc(sizeof(*metadata));
    REQUIRE(before && after && input && metadata);
    for (unsigned i = 0; i < 7000; ++i) input[i] = (uint16_t)(i * 59U);
    read_at(s, 32, before, 9000);
    memcpy(metadata, s, sizeof(*s));
    /* Pages0/4 exist; this write needs pages1/2/3. Fail each allocation. */
    for (unsigned fail = 1; fail <= 3; ++fail) {
        X64PmMetrics m;
        p->fail_at = p->attempts + fail;
        p->dirty_failure = 1;
        REQUIRE(x64_pm_write(s, 0, 128, input, 7000, &m) == X64_PM_NOMEM);
        REQUIRE(m.allocated_pages == fail - 1 && m.freed_pages == fail - 1);
        REQUIRE(!m.payload_bytes_written && !m.payload_bytes_copied);
        REQUIRE(!memcmp(s, metadata, sizeof(*s)) && p->live == 2);
        read_at(s, 32, after, 9000);
        REQUIRE(!memcmp(before, after, 9000 * sizeof(uint16_t)));
    }
    p->fail_at = 0;
    X64PmMetrics m;
    OK(x64_pm_write(s, 0, 128, input, 7000, &m));
    REQUIRE(m.allocated_pages == 3 && m.payload_bytes_written == 14000);
    read_at(s, 128, after, 7000);
    REQUIRE(!memcmp(input, after, 7000 * sizeof(uint16_t)));
    free(metadata); free(input); free(after); free(before);
    destroy(s); finish(p);
    puts("PASS write failure at every allocation, full rollback and retry");
}

static void clone_rollback_isolation(void) {
    Pool *p = pool(); X64PmSpace *src = space(p), *dst = space(p);
    const uint32_t at[] = {32, 63999, X64_PM_PRIVATE_CELLS - 1};
    uint16_t value = 0xabab;
    for (unsigned i = 0; i < 3; ++i) write_at(src, at[i], &value, 1);
    X64PmSpace *before_src = malloc(sizeof(*src)), *before_dst = malloc(sizeof(*dst));
    REQUIRE(before_src && before_dst);
    memcpy(before_src, src, sizeof(*src)); memcpy(before_dst, dst, sizeof(*dst));
    for (unsigned fail = 1; fail <= 3; ++fail) {
        X64PmMetrics m;
        p->fail_at = p->attempts + fail;
        REQUIRE(x64_pm_clone(dst, src, &m) == X64_PM_NOMEM);
        REQUIRE(m.allocated_pages == fail - 1 && m.freed_pages == fail - 1);
        REQUIRE(!m.payload_bytes_copied && p->live == 3);
        REQUIRE(!memcmp(before_src, src, sizeof(*src)) && !memcmp(before_dst, dst, sizeof(*dst)));
    }
    p->fail_at = 0;
    X64PmMetrics m;
    OK(x64_pm_clone(dst, src, &m));
    REQUIRE(m.allocated_pages == 3 && m.payload_bytes_copied == 3 * X64_PM_PAGE_BYTES);
    REQUIRE(x64_pm_clone(dst, src, NULL) == X64_PM_STATE);
    REQUIRE(x64_pm_clone(src, src, NULL) == X64_PM_STATE);
    for (unsigned i = 0; i < 3; ++i) {
        unsigned page = at[i] / X64_PM_PAGE_CELLS;
        REQUIRE(identity(src, page) != identity(dst, page));
        uint16_t out; read_at(dst, at[i], &out, 1); REQUIRE(out == value);
    }
    uint16_t changed = 0x1234; write_at(dst, at[1], &changed, 1);
    uint16_t out; read_at(src, at[1], &out, 1); REQUIRE(out == value);
    destroy(src);
    read_at(dst, at[1], &out, 1); REQUIRE(out == changed);
    OK(x64_pm_release(dst, NULL));
    REQUIRE(x64_pm_read(dst, 0, 32, &out, 1, NULL) == X64_PM_STATE);
    OK(x64_pm_create(dst, (X64PmOps){p, alloc4k, free4k}));
    read_at(dst, at[1], &out, 1); REQUIRE(out == 0);
    free(before_src); free(before_dst);
    destroy(dst); finish(p);
    puts("PASS eager clone allocation rollback, isolation and independent lifetime");
}

static void zero_and_reuse(void) {
    Pool *p = pool(); X64PmSpace *s = space(p), *empty = space(p);
    X64PmMetrics m;
    OK(x64_pm_zero(s, 0, 32, X64_PM_PRIVATE_CELLS - 32, &m));
    REQUIRE(!m.allocated_pages && !m.payload_bytes_written && !p->attempts);
    OK(x64_pm_clone(empty, s, &m)); REQUIRE(!m.allocated_pages && !m.payload_bytes_copied);
    destroy(empty);
    uint16_t data[] = {0x1234, 0xffff, 0xabcd, 0x8000}, out[4];
    write_at(s, 2047, data, 4);
    uint64_t first = identity(s, 0), second = identity(s, 1);
    OK(x64_pm_zero(s, 0, 2048, 2, &m));
    REQUIRE(!m.allocated_pages && m.payload_bytes_written == 4);
    read_at(s, 2047, out, 4);
    REQUIRE(out[0] == data[0] && !out[1] && !out[2] && out[3] == data[3]);
    REQUIRE(first == identity(s, 0) && second == identity(s, 1));
    for (unsigned i = 0; i < 30; ++i) {
        OK(x64_pm_release(s, NULL));
        OK(x64_pm_release(s, NULL));
        OK(x64_pm_create(s, (X64PmOps){p, alloc4k, free4k}));
        read_at(s, 2047, out, 4);
        REQUIRE(!out[0] && !out[1] && !out[2] && !out[3]);
        write_at(s, 2047, data, 4);
    }
    destroy(s); finish(p);
    puts("PASS sparse zero, stable frame identities, release and storage reuse");
}

static void bad_backend_shapes(void) {
    Pool *p = pool(); X64PmSpace *s = space(p);
    uint16_t value = 0x9876;
    for (unsigned bad = 1; bad <= 4; ++bad) {
        p->bad_shape = bad;
        X64PmMetrics m;
        REQUIRE(x64_pm_write(s, 0, 32, &value, 1, &m) == X64_PM_BACKING);
        REQUIRE(m.allocated_pages == 1 && m.freed_pages == 1 && !p->live && !s->allocated);
        uint16_t out; read_at(s, 32, &out, 1); REQUIRE(!out);
    }
    REQUIRE(x64_pm_create(s, (X64PmOps){p, alloc4k, free4k}) == X64_PM_STATE);
    REQUIRE(x64_pm_create(s, (X64PmOps){0}) == X64_PM_ARGUMENT);
    REQUIRE(x64_pm_create(NULL, (X64PmOps){p, alloc4k, free4k}) == X64_PM_ARGUMENT);
    destroy(s); finish(p);
    puts("PASS malformed backend shape cleanup and creation errors");
}

static void callback_reentry(void) {
    Pool *p = pool(); X64PmSpace *src = space(p), *dst = space(p);
    uint16_t value = 0x2222;
    p->probe[0] = src; write_at(src, 32, &value, 1);
    p->probe[1] = dst; OK(x64_pm_clone(dst, src, NULL));
    REQUIRE(p->busy_checks >= 3);
    p->probe[1] = NULL; OK(x64_pm_release(src, NULL));
    p->probe[0] = dst; OK(x64_pm_release(dst, NULL));
    p->probe[0] = NULL;
    destroy(src); destroy(dst); finish(p);
    puts("PASS participating spaces reject callback reentry during transactions");
}

static uint32_t random32(uint32_t *state) {
    *state = *state * 1664525U + 1013904223U;
    return *state;
}
static void oracle(void) {
    Pool *p = pool(); X64PmSpace *s = space(p);
    const unsigned cells = 100000;
    uint16_t *reference = calloc(cells, sizeof(uint16_t)), *out = malloc(cells * sizeof(uint16_t));
    REQUIRE(reference && out);
    uint32_t seed = 0x65abc4;
    for (unsigned round = 0; round < 600; ++round) {
        unsigned start = 32 + random32(&seed) % (cells - 32);
        unsigned n = random32(&seed) % 97;
        if (n > cells - start) n = cells - start;
        uint16_t values[97];
        for (unsigned i = 0; i < n; ++i) values[i] = (uint16_t)random32(&seed);
        if (round % 4) {
            write_at(s, start, values, n);
            memcpy(reference + start, values, n * sizeof(uint16_t));
        } else {
            OK(x64_pm_zero(s, start / 64000, start % 64000, n, NULL));
            memset(reference + start, 0, n * sizeof(uint16_t));
        }
        if (round % 71 == 0) {
            X64PmSpace *child = space(p);
            OK(x64_pm_clone(child, s, NULL));
            read_at(child, 32, out + 32, cells - 32);
            REQUIRE(!memcmp(reference + 32, out + 32, (cells - 32) * sizeof(uint16_t)));
            destroy(child);
        }
    }
    read_at(s, 32, out + 32, cells - 32);
    REQUIRE(!memcmp(reference + 32, out + 32, (cells - 32) * sizeof(uint16_t)));
    free(reference); free(out); destroy(s); finish(p);
    puts("PASS deterministic write/zero/clone differential oracle across 100000 cells");
}

static void dense_capacity(void) {
    Pool *p = pool(); X64PmSpace *s = space(p), *child = space(p);
    for (unsigned page = 0; page < X64_PM_PRIVATE_PAGES; ++page) {
        uint16_t value = (uint16_t)(page + 1);
        uint32_t at = page ? page * X64_PM_PAGE_CELLS : 32;
        write_at(s, at, &value, 1);
    }
    REQUIRE(p->live == X64_PM_PRIVATE_PAGES);
    X64PmMetrics m;
    OK(x64_pm_clone(child, s, &m));
    REQUIRE(m.allocated_pages == X64_PM_PRIVATE_PAGES);
    REQUIRE(m.payload_bytes_copied == (uint64_t)X64_PM_PRIVATE_PAGES * X64_PM_PAGE_BYTES);
    dense_clone_copied = m.payload_bytes_copied;
    for (unsigned page = 0; page < X64_PM_PRIVATE_PAGES; ++page) {
        uint16_t value;
        read_at(child, page ? page * X64_PM_PAGE_CELLS : 32, &value, 1);
        REQUIRE(value == page + 1 && identity(s, page) != identity(child, page));
    }
    X64PmMapping batch[64]; size_t total = 0;
    while (total < X64_PM_TOTAL_PAGES) {
        size_t count;
        OK(x64_pm_plan(child, (uint32_t)total, 64, batch, &count, &m));
        REQUIRE(!m.payload_bytes_copied && !m.payload_bytes_read && !m.payload_bytes_written);
        dense_plan_entries += m.entries_examined;
        for (size_t i = 0; i < count; ++i)
            REQUIRE(batch[i].region == (total + i < X64_PM_PRIVATE_PAGES ?
                        X64_PM_PRIVATE_BACKED : X64_PM_PLATFORM_SHARED));
        total += count;
    }
    destroy(child); destroy(s); finish(p);
    puts("PASS full 7500-frame private capacity, clone and 500 shared descriptors");
}

int main(void) {
    empty_and_plan(); boundaries(); rejected_spans(); transactional_write();
    clone_rollback_isolation(); zero_and_reuse(); bad_backend_shapes();
    callback_reentry(); oracle(); dense_capacity();
    printf("RESULT {\"groups\":%u,\"metadata_bytes\":%zu,\"private_pages\":%u,"
           "\"dense_clone_payload_bytes\":%llu,\"dense_plan_entries\":%llu,"
           "\"plan_payload_bytes_copied\":0,\"physical_context_switch_measured\":false}\n",
           groups, sizeof(X64PmSpace), X64_PM_PRIVATE_PAGES,
           (unsigned long long)dense_clone_copied, (unsigned long long)dense_plan_entries);
    return 0;
}
