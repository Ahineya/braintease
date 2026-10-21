/* Real high physical MMIO regression: QEMU pci-testdev BAR2 backs an otherwise
 * ordinary GOP-shaped buffer above 64 GiB. It is NOT a GPU/GOP firmware test.
 * The kernel runs its production console initialization on that buffer, checks
 * the remote end, then restores the actual display for the shell acceptance. */
#include "../boot/pci.h"
void high_framebuffer_prepare(void) {
    int bdf = pci_find(0x1b36, 0x0005);
    if (bdf < 0) fail("high framebuffer test device missing", 0);
    uint64_t address[6], size[6];
    pci_bars((unsigned)bdf, address, size);
    if (address[2] < PHYSICAL_LIMIT || size[2] != 16 * 1024 * 1024 ||
        (uint64_t)boot.stride * boot.height * 4 > size[2])
        fail("high framebuffer test BAR", address[2]);
    pci_write16((unsigned)bdf, 4, (uint16_t)pci_read((unsigned)bdf, 4) | 2);
    boot.framebuffer = address[2]; boot.framebuffer_size = size[2];
}
void high_framebuffer_check(void) {
    volatile uint32_t *pixels = (void *)boot.framebuffer;
    size_t end = (size_t)boot.stride * boot.height - 1;
    pixels[0] = 0x12345678; pixels[end] = 0x9abcdef0;
    if (pixels[0] != 0x12345678 || pixels[end] != 0x9abcdef0)
        fail("high framebuffer edge readback", boot.framebuffer);
    serial("HIGH_FRAMEBUFFER_TEST_OK base="); hex(boot.framebuffer); serial("\n");
}
