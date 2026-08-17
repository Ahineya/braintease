// 32-bit add from two unsigned shorts, with wrap carry.
// WAD column offsets are uint32; lump data can sit past 65535.

void putchar(int c);

int main() {
    unsigned short a_lo = 0xFFF0;
    unsigned short b_lo = 0x0020;
    unsigned short sum_lo = a_lo + b_lo;
    unsigned short carry = 0;
    unsigned short a_hi = 0x001D;
    unsigned short b_hi = 0x0000;
    unsigned short sum_hi;

    if (sum_lo < a_lo) {
        carry = 1;
    }
    sum_hi = a_hi + b_hi + carry;

    if (sum_lo == 0x0010) {
        putchar('Y');
    } else {
        putchar('N');
    }

    if (carry == 1) {
        putchar('Y');
    } else {
        putchar('N');
    }

    if (sum_hi == 0x001E) {
        putchar('Y');
    } else {
        putchar('N');
    }

    // No wrap: 1288 + 8
    a_lo = 1288;
    b_lo = 8;
    sum_lo = a_lo + b_lo;
    carry = 0;
    if (sum_lo < a_lo) {
        carry = 1;
    }
    if (sum_lo == 1296 && carry == 0) {
        putchar('Y');
    } else {
        putchar('N');
    }

    putchar('\n');
    return 0;
}
