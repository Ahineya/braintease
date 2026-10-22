/* Host callbacks exercise the exact inactive fixture with distinct physical
 * IDs and native aliases, plus externally owned sentinel pages. */
#include "../boot/process_memory_fixture.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGES 96U
#define BASE 0x100000ULL
static unsigned char *pool;
static unsigned char live[PAGES];
static unsigned allocated, freed;
static uint64_t kernel_sentinel = 0x123456789abcdef0ULL;
static uint64_t shared_sentinel = 0xfedcba9876543210ULL;
static uint64_t driver_map = 0x8000004800040183ULL;
static uint64_t guard_map = 0;
static unsigned selected_process = 17;
static unsigned index_of(uint64_t id) {
    assert(id >= BASE && !(id % 4096));
    unsigned i = (unsigned)((id - BASE) / 4096);
    assert(i < PAGES);
    return i;
}
static uint64_t alloc_page(void *opaque) {
    assert(opaque == pool);
    /* Leave 0 / last as separate pre-existing platform-owned guard storage. */
    for (unsigned i = 1; i + 1 < PAGES; ++i) {
        if (live[i]) continue;
        live[i] = 1; ++allocated;
        memset(pool + i * 4096, 0, 4096);
        return BASE + i * 4096;
    }
    return 0;
}
static void free_page(void *opaque, uint64_t id) {
    assert(opaque == pool);
    unsigned i = index_of(id);
    assert(i && i + 1 < PAGES && live[i]);
    live[i] = 0; ++freed;
    memset(pool + i * 4096, 0xdc, 4096);
}
static uint64_t available(void *opaque) {
    assert(opaque == pool);
    uint64_t n = 0;
    for (unsigned i = 1; i + 1 < PAGES; ++i) n += !live[i];
    return n;
}
static uint8_t *alias(void *opaque, uint64_t id) {
    assert(opaque == pool);
    return pool + index_of(id) * 4096;
}
static int allowed(void *opaque, uint64_t id, const uint8_t *bytes) {
    assert(opaque == pool);
    unsigned i = index_of(id);
    return i && i + 1 < PAGES && live[i] && bytes == pool + i * 4096;
}
static uint64_t audit(void *opaque) {
    assert(opaque == pool);
    uint64_t h = kernel_sentinel ^ shared_sentinel ^ driver_map ^ guard_map ^ selected_process;
    for (unsigned i = 0; i < 4096; ++i) h = h * 1099511628211ULL ^ pool[i];
    for (unsigned i = (PAGES - 1) * 4096; i < PAGES * 4096; ++i)
        h = h * 1099511628211ULL ^ pool[i];
    return h;
}
int main(void) {
    pool = aligned_alloc(4096, PAGES * 4096);
    assert(pool);
    memset(pool, 0x6d, PAGES * 4096);
    X64PmFixturePlatform p = {pool, BASE + PAGES * 4096,
        alloc_page, free_page, available, alias, allowed, audit};
    X64PmFixtureReport r;
    if (!x64_pm_fixture_run(&p, &r)) {
        fprintf(stderr, "fixture failed at line %u points=%llu\n", r.failed_check,
                (unsigned long long)r.allocation_points);
        return 1;
    }
    assert(r.allocation_points == 19 && r.injected_runs == r.allocation_points);
    assert(r.bad_plan_cases == 11 && r.mappings_walked == 16000);
    assert(allocated == freed && r.allocated_pages == r.freed_pages);
    assert(available(pool) == PAGES - 2);
    assert(kernel_sentinel == 0x123456789abcdef0ULL &&
           shared_sentinel == 0xfedcba9876543210ULL &&
           driver_map == 0x8000004800040183ULL && !guard_map && selected_process == 17);
    /* Repeatability catches persistent ledger / static workspace leftovers. */
    assert(x64_pm_fixture_run(&p, &r));
    printf("RESULT {\"allocation_points\":%llu,\"injected_runs\":%llu,"
        "\"bad_plan_cases\":%llu,\"mappings_walked\":%llu,"
        "\"plans_examined\":%llu,\"allocated_pages\":%llu,\"freed_pages\":%llu,"
        "\"inactive_only\":true,\"repeatability\":true}\n",
        (unsigned long long)r.allocation_points, (unsigned long long)r.injected_runs,
        (unsigned long long)r.bad_plan_cases, (unsigned long long)r.mappings_walked,
        (unsigned long long)r.plans_examined, (unsigned long long)r.allocated_pages,
        (unsigned long long)r.freed_pages);
    free(pool);
    return 0;
}
