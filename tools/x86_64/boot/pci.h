#ifndef RIPLIX_PCI_H
#define RIPLIX_PCI_H
#include "kernel.h"
uint32_t pci_read(unsigned bdf, unsigned offset);
void pci_write(unsigned bdf, unsigned offset, uint32_t value);
void pci_write16(unsigned bdf, unsigned offset, uint16_t value);
int pci_find(unsigned vendor, unsigned device);
int pci_find_class(unsigned code);
void pci_bars(unsigned bdf, uint64_t address[6], uint64_t size[6]);
#endif
