/* Isolated C16 backing model. No live page tables, CPU or scheduler access. */
#ifndef X64_PROCESS_MEMORY_H
#define X64_PROCESS_MEMORY_H
#include <stddef.h>
#include <stdint.h>

#define X64_PM_PAGE_BYTES 4096U
#define X64_PM_PAGE_CELLS 2048U
#define X64_PM_BANK_CELLS 64000U
#define X64_PM_PRIVATE_BANKS 240U
#define X64_PM_TOTAL_BANKS 256U
#define X64_PM_PRIVATE_CELLS (X64_PM_PRIVATE_BANKS * X64_PM_BANK_CELLS)
#define X64_PM_PRIVATE_PAGES 7500U
#define X64_PM_TOTAL_PAGES 8000U
#define X64_PM_DEVICE_CELLS 32U

typedef struct { uint64_t id; uint8_t *bytes; } X64PmBacking;
typedef struct {
    void *opaque;
    int (*alloc4k)(void *, X64PmBacking *out);
    void (*free4k)(void *, X64PmBacking backing);
} X64PmOps;

typedef enum {
    X64_PM_OK, X64_PM_ARGUMENT, X64_PM_STATE, X64_PM_RANGE,
    X64_PM_DEVICE, X64_PM_NOMEM, X64_PM_BACKING, X64_PM_BUSY
} X64PmError;

typedef struct {
    uint64_t allocated_pages, freed_pages;
    uint64_t payload_bytes_read, payload_bytes_written, payload_bytes_copied;
    uint64_t entries_examined;
} X64PmMetrics;

/* Caller-owned, zero-initialized storage. Fields are private to the module;
 * never shallow-copy a live space. Payloads are sparse; this index is fixed. */
typedef struct {
    X64PmBacking frames[X64_PM_PRIVATE_PAGES];
    uint8_t staged[(X64_PM_PRIVATE_PAGES + 7U) / 8U];
    X64PmOps ops;
    uint32_t state, busy, allocated;
} X64PmSpace;

typedef enum {
    X64_PM_PRIVATE_ZERO, X64_PM_PRIVATE_BACKED, X64_PM_PLATFORM_SHARED
} X64PmRegion;
typedef struct {
    uint64_t backing_id; /* Identity only, never an address to dereference. */
    uint32_t page;
    X64PmRegion region;
    uint32_t contains_device_cells; /* Page zero still requires checked I/O. */
} X64PmMapping;

/* Every metric output is optional and reset for this call, even on failure.
 * Caller buffers and metadata must be valid, disjoint from owned backing and
 * each other. Calls are externally serialized; callbacks cannot reenter a
 * participating space. No payload pointer may survive mutation or release. */
X64PmError x64_pm_create(X64PmSpace *, X64PmOps);
X64PmError x64_pm_read(const X64PmSpace *, uint32_t bank, uint32_t offset,
                      uint16_t *out, uint64_t cells, X64PmMetrics *);
X64PmError x64_pm_write(X64PmSpace *, uint32_t bank, uint32_t offset,
                       const uint16_t *in, uint64_t cells, X64PmMetrics *);
X64PmError x64_pm_zero(X64PmSpace *, uint32_t bank, uint32_t offset,
                      uint64_t cells, X64PmMetrics *);
/* Destination must already be created and empty. Failure leaves it empty. */
X64PmError x64_pm_clone(X64PmSpace *destination, X64PmSpace *source, X64PmMetrics *);
X64PmError x64_pm_release(X64PmSpace *, X64PmMetrics *);
/* Bounded snapshot of frame identities, not PTEs or permission grants.
 * References remain owned by the space, valid only until mutation/release.
 * Shared pages carry no id: the platform supplies their backing separately. */
X64PmError x64_pm_plan(const X64PmSpace *, uint32_t first_page, size_t capacity,
                      X64PmMapping *out, size_t *written, X64PmMetrics *);
#endif
