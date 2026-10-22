#ifndef RIPLIX_PROCESS_MEMORY_FIXTURE_H
#define RIPLIX_PROCESS_MEMORY_FIXTURE_H
#include <stdint.h>

/* Explicit acceptance-build hook only. The callbacks wrap ordinary releasable
 * platform pages; no mapping_page_alloc, active PTEs, CR3 or IRQ state changes.
 * Externally serialized. audit() fingerprints caller-selected live kernel,
 * shared-cell, guard, driver-map and process-selection sentinels. */
typedef struct {
    void *opaque;
    uint64_t physical_limit;
    uint64_t (*alloc_page)(void *);
    void (*free_page)(void *, uint64_t);
    uint64_t (*available_pages)(void *);
    uint8_t *(*identity_alias)(void *, uint64_t);
    int (*page_allowed)(void *, uint64_t, const uint8_t *);
    uint64_t (*audit)(void *);
} X64PmFixturePlatform;

typedef struct {
    uint64_t allocation_points, injected_runs, bad_plan_cases;
    uint64_t mappings_walked, plans_examined, allocated_pages, freed_pages;
    uint64_t available_before, available_after;
    uint32_t failed_check;
} X64PmFixtureReport;

/* Returns 1 on success. Failure still attempts teardown and records the first
 * failed source-line check. Static workspace: never call concurrently. */
int x64_pm_fixture_run(const X64PmFixturePlatform *, X64PmFixtureReport *);
#endif
