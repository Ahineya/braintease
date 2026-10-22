/* Small pure resource helpers. Platform discovery supplies evaluated root
 * windows and a complete occupied-span inventory; these helpers never perform
 * PCI config writes or infer that an unlisted address is safe to use. */
#ifndef RIPLIX_PCI_RESOURCE_H
#define RIPLIX_PCI_RESOURCE_H
#include <stddef.h>
#include <stdint.h>
typedef struct {uint64_t start,end;} PciResourceRange; /* end exclusive */
#define PCI_RESOURCE_MAX_WINDOWS 64
#define PCI_RESOURCE_MAX_OCCUPIED 2048
/* Selects highest aligned fit; no side effects, no relocation of other BARs.
 * Occupied spans must include assigned BARs, bridge windows, RAM, firmware and
 * reserved device resources. An incomplete inventory is unsafe at any address. */
const char *pci_resource_select(const PciResourceRange *windows,size_t count,
                               const PciResourceRange *occupied,size_t used,
                               uint64_t size,uint64_t alignment,uint64_t limit,
                               uint64_t *address);
#endif
