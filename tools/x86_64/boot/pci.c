/* PC configuration mechanism 1, PCI segment 0. Firmware assigns BARs/buses.
 * The timer ISR never accesses PCI, so address/data transactions cannot race. */
#include "pci.h"

uint32_t pci_read(unsigned bdf, unsigned offset) {
    out32(0xcf8, 0x80000000U | bdf << 8 | (offset & 252));
    return in32(0xcfc);
}
void pci_write(unsigned bdf, unsigned offset, uint32_t value) {
    out32(0xcf8, 0x80000000U | bdf << 8 | (offset & 252));
    out32(0xcfc, value);
}
void pci_write16(unsigned bdf, unsigned offset, uint16_t value) {
    out32(0xcf8, 0x80000000U | bdf << 8 | (offset & 252));
    /* Do not echo write-one-to-clear PCI status bits when changing command. */
    uint16_t port = 0xcfc + (offset & 2);
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}
static int find(unsigned value, unsigned class_code) {
    int found = -1;
    for (unsigned bus = 0; bus < 256; ++bus)
        for (unsigned slot = 0; slot < 32; ++slot) {
            unsigned first = bus << 8 | slot << 3;
            if ((pci_read(first, 0) & 65535) == 65535) continue;
            unsigned functions = pci_read(first, 12) & 0x800000 ? 8 : 1;
            for (unsigned fn = 0; fn < functions; ++fn) {
                unsigned bdf = first | fn;
                if (class_code ? pci_read(bdf, 8) >> 8 != value : pci_read(bdf, 0) != value) continue;
                if (found >= 0) fail("multiple root block devices", bdf);
                found = (int)bdf;
            }
        }
    return found;
}
int pci_find(unsigned vendor, unsigned device) { return find(device << 16 | vendor, 0); }
int pci_find_class(unsigned code) { return find(code, 1); }
void pci_bars(unsigned bdf, uint64_t address[6], uint64_t size[6]) {
    if ((pci_read(bdf, 12) >> 16 & 127) != 0) fail("PCI endpoint header", bdf);
    uint16_t command = pci_read(bdf, 4);
    pci_write16(bdf, 4, command & ~7U);
    for (unsigned i = 0; i < 6; ++i) {
        address[i] = size[i] = 0;
        unsigned offset = 16 + i * 4;
        uint32_t low = pci_read(bdf, offset);
        if (low & 1) continue; /* This driver uses memory BARs only. */
        unsigned wide = (low & 6) == 4;
        if ((low & 6) && !wide) fail("unsupported PCI BAR type", low);
        if (wide && i == 5) fail("truncated PCI 64-bit BAR", bdf);
        uint32_t high = wide ? pci_read(bdf, offset + 4) : 0;
        pci_write(bdf, offset, UINT32_MAX);
        if (wide) pci_write(bdf, offset + 4, UINT32_MAX);
        uint64_t mask = pci_read(bdf, offset) & ~15U;
        if (wide) mask |= (uint64_t)pci_read(bdf, offset + 4) << 32;
        pci_write(bdf, offset, low);
        if (wide) pci_write(bdf, offset + 4, high);
        if (mask) {
            address[i] = ((uint64_t)high << 32) | (low & ~15U);
            size[i] = wide ? ~mask + 1 : (uint32_t)(~(uint32_t)mask + 1);
            if (!size[i] || (size[i] & (size[i] - 1))) fail("PCI BAR size", size[i]);
        }
        if (wide) { ++i; address[i] = size[i] = 0; }
    }
    pci_write16(bdf, 4, command);
}
