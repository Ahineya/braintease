#include <stdio.h>
#include <mmio.h>

unsigned short byte_at(unsigned short block, unsigned short byte_addr) {
    unsigned short word = storage_read_at(block, byte_addr >> 1);
    if (byte_addr & 1) {
        return (word >> 8) & 0xFF;
    }
    return word & 0xFF;
}

int main() {
    storage_write_at(1, 0, 'I' | ('W' << 8));
    storage_write_at(1, 1, 'A' | ('D' << 8));

    if (byte_at(1, 0) == 'I') putchar('Y'); else putchar('N');
    if (byte_at(1, 1) == 'W') putchar('Y'); else putchar('N');
    if (byte_at(1, 2) == 'A') putchar('Y'); else putchar('N');
    if (byte_at(1, 3) == 'D') putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
