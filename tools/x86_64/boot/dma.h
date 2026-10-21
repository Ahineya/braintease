#ifndef RIPLIX_DMA_H
#define RIPLIX_DMA_H
#include "acpi.h"
#define DMA_MAX_UNITS 16
#define DMA_MAX_RMRRS 32
typedef struct {
    unsigned unit_count, rmrr_count;
    uint64_t units[DMA_MAX_UNITS], ecam;
    struct { uint64_t first, last; } rmrr[DMA_MAX_RMRRS];
} DmaInfo;
/* Parse only. No MMIO or PCI side effects. Firmware tables were checksummed
 * by ACPI discovery; lengths are rechecked here before accessing records. */
const char *dma_parse(const AcpiInfo *, unsigned bdf, DmaInfo *);
/* Fail closed before enabling the selected device's bus mastering. Does NOT
 * disable/reprogram an IOMMU or change firmware translations. */
const char *dma_identity_check(unsigned bdf);
#endif
