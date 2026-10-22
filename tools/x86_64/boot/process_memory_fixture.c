/* Inactive allocator / backing / 4-KiB PTE construction acceptance fixture.
 * This source is deliberately absent from normal builds. */
#ifndef X64_PROCESS_MEMORY_FIXTURE
#error "process memory fixture requires an explicit acceptance build"
#endif
#include "process_memory_fixture.h"
#include "../process-memory/process_memory.h"
#include <stddef.h>

#define LIMIT 64U
#define BATCH 31U
#define PAGE 4096ULL
#define ADDRESS 0x000ffffffffff000ULL
#define PRESENT 1ULL
#define WRITABLE 2ULL
#define TABLE_OWNER 16U
#define SHARED_OWNER 32U

typedef struct { uint64_t id; uint8_t *bytes; unsigned owner; } Allocation;
typedef struct {
    X64PmSpace model;
    uint64_t generation;
    unsigned owner;
} Space;
typedef struct { uint64_t root, generation; unsigned owner; } Image;
typedef struct {
    X64PmFixturePlatform platform;
    X64PmFixtureReport *report;
    Space spaces[2];
    Allocation allocations[LIMIT];
    unsigned live;
    uint64_t attempts, fail_at;
    X64PmBacking shared;
    unsigned callback_error;
} Workspace;
static Workspace work;

static void clear(void *p, size_t n) {
    uint8_t *b = p;
    while (n--) *b++ = 0;
}
static int check(int condition, unsigned line) {
    if (!condition && !work.report->failed_check) work.report->failed_check = line;
    return condition;
}
#define REQUIRE(c) do { if (!check((c), __LINE__)) goto done; } while (0)
static Allocation *lookup(uint64_t id) {
    for (unsigned i = 0; i < LIMIT; ++i)
        if (work.allocations[i].id == id) return &work.allocations[i];
    return NULL;
}
static Allocation *lookup_owner(uint64_t id, unsigned owner) {
    Allocation *a = lookup(id);
    return a && a->owner == owner ? a : NULL;
}
static int acquire(unsigned owner, X64PmBacking *out) {
    *out = (X64PmBacking){0};
    ++work.attempts;
    if (work.fail_at == work.attempts) return 0;
    uint64_t id = work.platform.alloc_page(work.platform.opaque);
    if (!id) return 0;
    /* The platform allocator is trusted to return an exclusive allocation.
     * Reject malformed/foreign ownership without ever freeing a foreign ID. */
    if (!id || id % PAGE || id >= work.platform.physical_limit || lookup(id)) {
        work.callback_error = 1;
        return 0;
    }
    uint8_t *bytes = work.platform.identity_alias(work.platform.opaque, id);
    if (!bytes || (uintptr_t)bytes % PAGE ||
            !work.platform.page_allowed(work.platform.opaque, id, bytes)) {
        /* This is a genuine allocation with a bad alias/provenance result;
         * return the exact ID through its matching platform callback. */
        work.platform.free_page(work.platform.opaque, id);
        work.callback_error = 1;
        return 0;
    }
    for (unsigned i = 0; i < LIMIT; ++i) {
        if (work.allocations[i].id && work.allocations[i].bytes == bytes) {
            work.platform.free_page(work.platform.opaque, id);
            work.callback_error = 1;
            return 0;
        }
    }
    for (unsigned n = 0; n < PAGE; ++n) {
        if (bytes[n]) {
            work.platform.free_page(work.platform.opaque, id);
            work.callback_error = 1;
            return 0;
        }
    }
    for (unsigned i = 0; i < LIMIT; ++i) {
        if (work.allocations[i].id) continue;
        work.allocations[i] = (Allocation){id, bytes, owner};
        ++work.live;
        ++work.report->allocated_pages;
        *out = (X64PmBacking){id, bytes};
        return 1;
    }
    work.platform.free_page(work.platform.opaque, id);
    work.callback_error = 1;
    return 0;
}
static int relinquish(uint64_t id, unsigned owner) {
    Allocation *a = lookup_owner(id, owner);
    if (!a) { work.callback_error = 1; return 0; }
    work.platform.free_page(work.platform.opaque, a->id);
    *a = (Allocation){0};
    --work.live;
    ++work.report->freed_pages;
    return 1;
}
static int alloc_backing(void *opaque, X64PmBacking *out) {
    Space *s = opaque;
    /* A deliberately small per-space payload budget, independent of table
     * and fixture-shared ownership. clone/write must fail atomically at it. */
    if (s->model.allocated >= 4U) return 0;
    return acquire(s->owner, out);
}
static void free_backing(void *opaque, X64PmBacking b) {
    Space *s = opaque;
    Allocation *a = lookup_owner(b.id, s->owner);
    if (!a || a->bytes != b.bytes) { work.callback_error = 1; return; }
    (void)relinquish(b.id, s->owner);
}
static uint64_t payload_digest(const Space *s) {
    uint64_t digest = 1469598103934665603ULL;
    for (unsigned p = 0; p < X64_PM_PRIVATE_PAGES; ++p) {
        X64PmBacking b = s->model.frames[p];
        if (!b.id) continue;
        digest = digest * 1099511628211ULL ^ b.id;
        for (unsigned i = 0; i < PAGE; ++i)
            digest = digest * 1099511628211ULL ^ b.bytes[i];
    }
    return digest;
}
static int write_cells(Space *s, uint32_t bank, uint32_t offset,
                       const uint16_t *values, uint64_t count) {
    uint64_t before = payload_digest(s);
    unsigned allocated = s->model.allocated;
    X64PmError e = x64_pm_write(&s->model, bank, offset, values, count, NULL);
    if (e) (void)check(payload_digest(s) == before && s->model.allocated == allocated, __LINE__);
    if (!e) ++s->generation;
    return e == X64_PM_OK;
}
static int read_cells(Space *s, uint32_t bank, uint32_t offset,
                      const uint16_t *values, uint64_t count) {
    uint16_t actual[2] = {0};
    if (count > 2 || x64_pm_read(&s->model, bank, offset, actual, count, NULL)) return 0;
    for (uint64_t i = 0; i < count; ++i) if (actual[i] != values[i]) return 0;
    return 1;
}
static int entry_valid(const Space *s, const X64PmMapping *m, uint32_t page) {
    if (m->page != page || m->contains_device_cells != (page == 0)) return 0;
    if (page >= X64_PM_PRIVATE_PAGES)
        return m->region == X64_PM_PLATFORM_SHARED && m->backing_id == 0;
    uint64_t expected = s->model.frames[page].id;
    if (!expected) return m->region == X64_PM_PRIVATE_ZERO && !m->backing_id;
    if (m->region != X64_PM_PRIVATE_BACKED || m->backing_id != expected) return 0;
    for (uint32_t before = 0; before < page; ++before)
        if (s->model.frames[before].id == m->backing_id) return 0;
    Allocation *a = lookup_owner(m->backing_id, s->owner);
    return a && a->bytes == s->model.frames[page].bytes &&
        work.platform.page_allowed(work.platform.opaque, a->id, a->bytes);
}
static uint64_t *table(uint64_t id, unsigned owner) {
    Allocation *a = lookup_owner(id, owner);
    return a ? (uint64_t *)(void *)a->bytes : NULL;
}
static int map_page(Image *image, uint32_t page, uint64_t frame, int writable) {
    uint64_t va = (uint64_t)page * PAGE;
    uint64_t *at = table(image->root, image->owner);
    if (!at) return 0;
    for (unsigned level = 3; level; --level) {
        unsigned slot = (unsigned)((va >> (12U + level * 9U)) & 511U);
        if (!at[slot]) {
            X64PmBacking b;
            if (!acquire(image->owner, &b)) return 0;
            at[slot] = b.id | PRESENT | WRITABLE;
        }
        if ((at[slot] & ~ADDRESS) != (PRESENT | WRITABLE)) return 0;
        at = table(at[slot] & ADDRESS, image->owner);
        if (!at) return 0;
    }
    unsigned slot = (unsigned)((va >> 12) & 511U);
    if (at[slot] || !frame || frame % PAGE) return 0;
    at[slot] = frame | PRESENT | (writable ? WRITABLE : 0);
    return 1;
}
static int stage_image(Space *s, uint64_t generation, Image *out) {
    *out = (Image){0, generation, s->owner + TABLE_OWNER};
    if (generation != s->generation) return 0;
    X64PmBacking b;
    if (!acquire(out->owner, &b)) return 0;
    out->root = b.id;
    X64PmMapping batch[BATCH];
    for (uint32_t first = 0; first < X64_PM_TOTAL_PAGES;) {
        size_t written = 0;
        if (generation != s->generation ||
                x64_pm_plan(&s->model, first, BATCH, batch, &written, NULL) || !written) return 0;
        for (size_t n = 0; n < written; ++n) {
            uint32_t page = first + (uint32_t)n;
            ++work.report->plans_examined;
            if (!entry_valid(s, &batch[n], page)) return 0;
            if (batch[n].region == X64_PM_PRIVATE_BACKED) {
                if (!map_page(out, page, batch[n].backing_id, 1)) return 0;
            } else if (page == X64_PM_PRIVATE_PAGES) {
                /* Separately supplied fixture-shared source. This is not an
                 * alias of Context.cells, nor a live shared-bank migration. */
                Allocation *shared = lookup_owner(work.shared.id, SHARED_OWNER);
                if (!shared || shared->bytes != work.shared.bytes ||
                        !map_page(out, page, shared->id, 0)) return 0;
            }
        }
        first += (uint32_t)written;
    }
    return generation == s->generation;
}
static int walk(const Image *image, uint32_t page, uint64_t *frame, uint64_t *flags) {
    uint64_t va = (uint64_t)page * PAGE;
    uint64_t *at = table(image->root, image->owner);
    if (!at) return 0;
    for (unsigned level = 3; level; --level) {
        uint64_t pte = at[(va >> (12U + level * 9U)) & 511U];
        if (!pte) { *frame = *flags = 0; return 1; }
        if ((pte & ~ADDRESS) != (PRESENT | WRITABLE)) return 0;
        at = table(pte & ADDRESS, image->owner);
        if (!at) return 0;
    }
    uint64_t pte = at[(va >> 12) & 511U];
    *frame = pte & ADDRESS;
    *flags = pte & ~ADDRESS;
    return !pte || (*flags == PRESENT || *flags == (PRESENT | WRITABLE));
}
static int validate_image(const Space *s, const Image *image) {
    if (image->generation != s->generation) return 0;
    for (uint32_t page = 0; page < X64_PM_TOTAL_PAGES; ++page) {
        uint64_t frame = 0, flags = 0;
        if (!walk(image, page, &frame, &flags)) return 0;
        ++work.report->mappings_walked;
        uint64_t expected = page < X64_PM_PRIVATE_PAGES ? s->model.frames[page].id :
            page == X64_PM_PRIVATE_PAGES ? work.shared.id : 0;
        uint64_t wanted_flags = !expected ? 0 : page < X64_PM_PRIVATE_PAGES ? 3 : 1;
        if (frame != expected || flags != wanted_flags) return 0;
    }
    return 1;
}
static void release_tables(unsigned owner) {
    for (unsigned i = 0; i < LIMIT; ++i)
        if (work.allocations[i].id && work.allocations[i].owner == owner)
            (void)relinquish(work.allocations[i].id, owner);
}
static int reject_bad_plans(Space *s, Space *other, Image *image) {
    X64PmMapping m;
    size_t n;
    if (x64_pm_plan(&s->model, 0, 1, &m, &n, NULL) || n != 1) return 0;
    X64PmMapping bad = m;
    bad.backing_id = other->model.frames[0].id;
    if (entry_valid(s, &bad, 0)) return 0; /* valid frame, wrong space */
    ++work.report->bad_plan_cases;
    bad = m; bad.backing_id += 1;
    if (entry_valid(s, &bad, 0)) return 0; /* unaligned/unknown frame */
    ++work.report->bad_plan_cases;
    bad = m; bad.page = 1;
    if (entry_valid(s, &bad, 0)) return 0; /* overlap / wrong plan position */
    ++work.report->bad_plan_cases;
    bad = m; bad.contains_device_cells = 0;
    if (entry_valid(s, &bad, 0)) return 0;
    ++work.report->bad_plan_cases;
    bad = m; bad.backing_id = work.shared.id;
    if (entry_valid(s, &bad, 0)) return 0; /* shared cannot masquerade private */
    ++work.report->bad_plan_cases;
    /* Corrupt borrowed metadata, never foreign platform storage. Restore it
     * before releasing; ledger ownership remains the sole source of frees. */
    X64PmBacking saved = s->model.frames[0];
    s->model.frames[0].bytes += 2;
    int rejected = !entry_valid(s, &m, 0);
    s->model.frames[0] = saved;
    if (!rejected) return 0;
    ++work.report->bad_plan_cases;
    X64PmBacking saved_one = s->model.frames[1];
    s->model.frames[1] = saved;
    bad = m; bad.page = 1; bad.contains_device_cells = 0;
    /* A second position cannot reuse any preceding private frame ID. */
    rejected = !entry_valid(s, &bad, 1);
    s->model.frames[1] = saved_one;
    if (!rejected) return 0;
    ++work.report->bad_plan_cases;
    /* PTE permission/provenance validation, including table-vs-payload IDs. */
    uint64_t *root = table(image->root, image->owner);
    uint64_t root_entry = root[0];
    root[0] = s->model.frames[0].id | 3;
    if (validate_image(s, image)) { root[0] = root_entry; return 0; }
    root[0] = root_entry;
    ++work.report->bad_plan_cases;
    uint64_t *leaf = root;
    for (unsigned level = 3; level; --level) {
        leaf = table(leaf[0] & ADDRESS, image->owner);
        if (!leaf) return 0;
    }
    uint64_t leaf_entry = leaf[0];
    leaf[0] |= 4; /* user bit must never appear */
    if (validate_image(s, image)) { leaf[0] = leaf_entry; return 0; }
    leaf[0] = leaf_entry;
    ++work.report->bad_plan_cases;
    Image stale = *image;
    --stale.generation;
    if (validate_image(s, &stale)) return 0;
    ++work.report->bad_plan_cases;
    Image empty;
    if (stage_image(s, s->generation - 1, &empty) || empty.root) return 0;
    ++work.report->bad_plan_cases;
    return 1;
}

/* One deterministic run. Nth acquisition failure is injected before the real
 * callback, covering shared, write, clone, root and every subordinate table. */
static int run_once(uint64_t fail_at, uint64_t *points) {
    const uint16_t one[] = {0xff80}, edge[] = {0x8001, 0xfffe};
    const uint16_t bank[] = {0x8000, 0xffff}, last[] = {0xe987};
    const uint16_t changed[] = {0x8765};
    const uint16_t zero[] = {0};
    int success = 0;
    clear(work.spaces, sizeof(work.spaces));
    clear(work.allocations, sizeof(work.allocations));
    work.live = work.callback_error = 0;
    work.shared = (X64PmBacking){0};
    work.attempts = 0; work.fail_at = fail_at;
    uint64_t before = work.platform.available_pages(work.platform.opaque);
    uint64_t audit = work.platform.audit(work.platform.opaque);
    Space *a = &work.spaces[0], *b = &work.spaces[1];
    Image images[2] = {{0}};
    a->owner = 1; b->owner = 2;
    REQUIRE(x64_pm_create(&a->model, (X64PmOps){a, alloc_backing, free_backing}) == X64_PM_OK);
    REQUIRE(x64_pm_create(&b->model, (X64PmOps){b, alloc_backing, free_backing}) == X64_PM_OK);
    if (!acquire(SHARED_OWNER, &work.shared)) goto done;
    work.shared.bytes[0] = 0xa5; work.shared.bytes[PAGE - 1] = 0x5a;
    if (!write_cells(a, 0, 32, one, 1) || !write_cells(a, 0, 2047, edge, 2) ||
            !write_cells(a, 0, 63999, bank, 2) || !write_cells(a, 239, 63999, last, 1)) goto done;
    REQUIRE(a->model.allocated == 4);
    REQUIRE(read_cells(a, 0, 32, one, 1) && read_cells(a, 0, 2047, edge, 2) &&
        read_cells(a, 0, 63999, bank, 2) && read_cells(a, 239, 63999, last, 1));
    REQUIRE(x64_pm_read(&a->model, 0, 0, NULL, 1, NULL) == X64_PM_DEVICE);
    /* Over budget write cannot mutate an existing payload or materialize the
     * requested hole. It also must not call the physical allocator. */
    uint64_t attempts = work.attempts;
    REQUIRE(x64_pm_write(&a->model, 0, 4096, one, 1, NULL) == X64_PM_NOMEM);
    REQUIRE(work.attempts == attempts && read_cells(a, 0, 4096, zero, 1));
    const uint16_t unchanged[] = {0, 0};
    uint64_t digest = payload_digest(a);
    REQUIRE(!write_cells(a, 0, 4095, edge, 2));
    REQUIRE(work.attempts == attempts && read_cells(a, 0, 4095, unchanged, 2));
    if (x64_pm_clone(&b->model, &a->model, NULL)) {
        REQUIRE(!b->model.allocated && payload_digest(a) == digest);
        goto done;
    }
    ++b->generation;
    REQUIRE(b->model.allocated == 4);
    REQUIRE(write_cells(b, 0, 32, changed, 1));
    REQUIRE(read_cells(a, 0, 32, one, 1) && read_cells(b, 0, 32, changed, 1));
    if (!stage_image(a, a->generation, &images[0]) ||
            !stage_image(b, b->generation, &images[1])) goto done;
    REQUIRE(validate_image(a, &images[0]) && validate_image(b, &images[1]));
    REQUIRE(reject_bad_plans(a, b, &images[0]));
    REQUIRE(work.shared.bytes[0] == 0xa5 && work.shared.bytes[PAGE - 1] == 0x5a);
    success = 1;
done:
    if (points) *points = work.attempts;
    release_tables(TABLE_OWNER + 1);
    release_tables(TABLE_OWNER + 2);
    (void)check(x64_pm_release(&a->model, NULL) == X64_PM_OK, __LINE__);
    (void)check(x64_pm_release(&b->model, NULL) == X64_PM_OK, __LINE__);
    if (work.shared.id) {
        (void)check(work.shared.bytes[0] == 0xa5 && work.shared.bytes[PAGE - 1] == 0x5a, __LINE__);
        (void)relinquish(work.shared.id, SHARED_OWNER);
    }
    (void)check(!work.callback_error && !work.live, __LINE__);
    (void)check(work.platform.available_pages(work.platform.opaque) == before, __LINE__);
    (void)check(work.platform.audit(work.platform.opaque) == audit, __LINE__);
    if (fail_at) (void)check(!success && work.attempts == fail_at, __LINE__);
    return success && !work.report->failed_check;
}
int x64_pm_fixture_run(const X64PmFixturePlatform *platform, X64PmFixtureReport *report) {
    if (!platform || !report || !platform->alloc_page || !platform->free_page ||
            !platform->available_pages || !platform->identity_alias ||
            !platform->page_allowed || !platform->audit || platform->physical_limit < PAGE * 2) return 0;
    clear(&work, sizeof(work)); clear(report, sizeof(*report));
    work.platform = *platform; work.report = report;
    report->available_before = platform->available_pages(platform->opaque);
    if (!run_once(0, &report->allocation_points)) {
        (void)check(0, __LINE__);
        return 0;
    }
    for (uint64_t at = 1; at <= report->allocation_points; ++at) {
        (void)run_once(at, NULL);
        if (report->failed_check) return 0;
        ++report->injected_runs;
    }
    report->available_after = platform->available_pages(platform->opaque);
    return check(report->allocated_pages == report->freed_pages &&
                 report->available_before == report->available_after, __LINE__);
}
