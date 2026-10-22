#ifndef RIPLIX_PCI_ASSIGN_H
#define RIPLIX_PCI_ASSIGN_H
#include <stdint.h>
/* Assign only an unassigned 64-bit BAR0 on the direct root bus. Existing
 * firmware assignments and every other device remain untouched. */
const char *pci_assign_bar0(unsigned bdf,uint64_t minimum,uint64_t *address);
const char *pci_mmio_bounds(uint64_t address,uint64_t bytes);
#endif
