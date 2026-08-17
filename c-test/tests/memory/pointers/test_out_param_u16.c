// add32-style out-parameters through unsigned short*.
// Same translation unit (cross-object pointers were unreliable).

void putchar(int c);

void add32(unsigned short a_hi, unsigned short a_lo,
           unsigned short b_hi, unsigned short b_lo,
           unsigned short *out_hi, unsigned short *out_lo) {
    unsigned short sum_lo = a_lo + b_lo;
    unsigned short carry = 0;
    if (sum_lo < a_lo) {
        carry = 1;
    }
    *out_lo = sum_lo;
    *out_hi = a_hi + b_hi + carry;
}

int main() {
    unsigned short hi;
    unsigned short lo;

    add32(0x001D, 0xFFF0, 0x0000, 0x0020, &hi, &lo);
    if (lo == 0x0010) {
        putchar('Y');
    } else {
        putchar('N');
    }
    if (hi == 0x001E) {
        putchar('Y');
    } else {
        putchar('N');
    }

    add32(0x001D, 0x7EE0, 0x0000, 1288, &hi, &lo);
    if (lo == 0x83E8) {
        putchar('Y');
    } else {
        putchar('N');
    }
    if (hi == 0x001D) {
        putchar('Y');
    } else {
        putchar('N');
    }

    putchar('\n');
    return 0;
}
