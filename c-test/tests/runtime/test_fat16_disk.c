#include <stdio.h>
#include <stdint.h>
#include <mmio.h>
#include "../../examples/rOS/disk.c"

int main() {
    uint8_t b;
    uint16_t w;
    uint32_t d;

    disk_write8(100, 'A');
    disk_write8(101, 'B');
    disk_write8(102, 'C');
    disk_write8(103, 'D');

    b = disk_read8(100);
    if (b == 'A') putchar('Y'); else putchar('N');
    b = disk_read8(101);
    if (b == 'B') putchar('Y'); else putchar('N');
    b = disk_read8(102);
    if (b == 'C') putchar('Y'); else putchar('N');
    b = disk_read8(103);
    if (b == 'D') putchar('Y'); else putchar('N');

    w = disk_read16(100);
    if (w == ('A' | ('B' << 8))) putchar('Y'); else putchar('N');

    disk_write16(200, 0x1234);
    w = disk_read16(200);
    if (w == 0x1234) putchar('Y'); else putchar('N');

    disk_write32(300, 0x89ABCDEFUL);
    d = disk_read32(300);
    if (d == 0x89ABCDEFUL) putchar('Y'); else putchar('N');

    disk_write8(65535UL, 0x11);
    disk_write8(65536UL, 0x22);
    if (disk_read8(65535UL) == 0x11) putchar('Y'); else putchar('N');
    if (disk_read8(65536UL) == 0x22) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
