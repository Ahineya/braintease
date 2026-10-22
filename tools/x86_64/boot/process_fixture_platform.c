#ifndef X64_PROCESS_MEMORY_FIXTURE
#error "process memory platform fixture requires an explicit acceptance build"
#endif
#include "kernel.h"
#include "process_fixture_platform.h"
#include "process_memory_fixture.h"

typedef struct {
    uint64_t (*sentinel)(void *);
    void *opaque;
} PlatformAudit;
static uint64_t allocate(void *opaque) { (void)opaque; return page_alloc(); }
static void release(void *opaque, uint64_t id) { (void)opaque; page_free(id); }
static uint64_t available(void *opaque) { (void)opaque; return page_available(); }
static uint8_t *alias(void *opaque, uint64_t id) {
    (void)opaque;
    return (uint8_t *)(uintptr_t)id;
}
static int overlaps(uint64_t page, uint64_t start, uint64_t bytes) {
    if (!bytes) return 0;
    if (start > UINT64_MAX - bytes) return 1;
    return page < start + bytes && page + 4096 > start;
}
static int allowed(void *opaque, uint64_t id, const uint8_t *bytes) {
    (void)opaque;
    if (!id || id % 4096 || id > PHYSICAL_LIMIT - 4096 ||
            bytes != (const uint8_t *)(uintptr_t)id) return 0;
    if (boot.kernel_end < boot.kernel_start ||
            overlaps(id, boot.kernel_start, boot.kernel_end - boot.kernel_start)) return 0;
    /* Allocator ownership comes exclusively from page_alloc. These additional
     * checks fail closed if its identity alias were ever handed device/image
     * provenance. No BAR/device mapping is created or made releasable here. */
    if (overlaps(id, boot.framebuffer, boot.framebuffer_size) ||
            overlaps(id, boot.memory_map, boot.memory_map_size)) return 0;
    return 1;
}
static uint64_t audit(void *opaque) {
    PlatformAudit *context = opaque;
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3) : : "memory");
    uint64_t h = context->sentinel(context->opaque);
    h = h * 1099511628211ULL ^ cr3;
    h = h * 1099511628211ULL ^ page_mapping_owned();
    return h;
}
void process_fixture_platform_run(uint64_t (*sentinel)(void *), void *opaque) {
    if (!sentinel) fail("process fixture missing sentinel", 0);
    PlatformAudit context = {sentinel, opaque};
    X64PmFixturePlatform platform = {&context, PHYSICAL_LIMIT, allocate, release,
        available, alias, allowed, audit};
    X64PmFixtureReport report;
    if (!x64_pm_fixture_run(&platform, &report))
        fail("inactive process-memory fixture", report.failed_check);
    serial("PROCESS_MEMORY_FIXTURE_OK allocations="); hex(report.allocation_points);
    serial(" injected="); hex(report.injected_runs);
    serial(" bad_plans="); hex(report.bad_plan_cases);
    serial(" walked="); hex(report.mappings_walked);
    serial(" allocated="); hex(report.allocated_pages);
    serial(" freed="); hex(report.freed_pages);
    serial(" inactive_root=1\n");
}
