#ifndef RIPLIX_PCI_REFINE_H
#define RIPLIX_PCI_REFINE_H
/* Once, after ACPI discovery with interrupts disabled, before device owners.
 * Refines firmware reservations only; never assigns or maps an address.
 * A failure preserves unproven reservations and marks the inventory incomplete. */
const char *pci_resources_refine(void);
#endif
