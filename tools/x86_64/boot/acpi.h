/* Native firmware metadata, independent of the guest C16 address space. */
#ifndef RIPLIX_X86_ACPI_H
#define RIPLIX_X86_ACPI_H
#include <stddef.h>
#include <stdint.h>
#include "power.h"
#define ACPI_MAX_CPUS 256
#define ACPI_MAX_IOAPICS 8
typedef struct {
    uint64_t lapic;
    const uint8_t *dmar, *mcfg;
    uint32_t dmar_length, mcfg_length;
    unsigned ivrs_present;
    unsigned pcat, cpu_count, io_count, override_count;
    uint32_t cpu_ids[ACPI_MAX_CPUS];
    struct { uint32_t address, gsi; uint8_t id; } io[ACPI_MAX_IOAPICS];
    struct { uint32_t gsi; uint16_t flags; uint8_t present; } isa[16];
    unsigned fadt_present, boot_arch;
    PowerReset reset;
} AcpiInfo;
/* A mapping callback must validate the entire physical span before returning
 * a pointer. It cannot assume ACPI firmware lengths or pointers are sound. */
typedef const uint8_t *(*AcpiMap)(void *, uint64_t, size_t);
const char *acpi_discover(uint64_t rsdp, AcpiMap map, void *context, AcpiInfo *out);
extern AcpiInfo acpi;
void acpi_init(void);
#endif
