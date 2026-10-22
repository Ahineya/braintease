#ifndef RIPLIX_PCI_UEFI_H
#define RIPLIX_PCI_UEFI_H
#include "bootinfo.h"
#include "uefi.h"
/* Call before final GetMemoryMap/ExitBootServices. A partial/unsupported
 * inventory is diagnostics only: native resource assignment must require
 * boot.pci.status == PCI_BOOT_COMPLETE. No PCI writes or probing occur here. */
void pci_uefi_capture(BootServices *, BootInfo *);
#endif
