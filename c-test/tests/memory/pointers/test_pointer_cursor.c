// WAD cursor as (hi, lo) updated through pointers, including 16-bit wrap.

void putchar(int c);

void seek_to(unsigned short *hi, unsigned short *lo,
             unsigned short to_hi, unsigned short to_lo) {
    *hi = to_hi;
    *lo = to_lo;
}

void skip(unsigned short *hi, unsigned short *lo, unsigned short n) {
    unsigned short old = *lo;
    unsigned short next = old + n;
    *lo = next;
    if (next < old) {
        *hi = *hi + 1;
    }
}

int main() {
    unsigned short cur_hi;
    unsigned short cur_lo;

    seek_to(&cur_hi, &cur_lo, 0, 0);
    if (cur_hi == 0 && cur_lo == 0) {
        putchar('Y');
    } else {
        putchar('N');
    }

    seek_to(&cur_hi, &cur_lo, 0x003F, 0xB7B4);
    skip(&cur_hi, &cur_lo, 16);
    if (cur_hi == 0x003F && cur_lo == 0xB7C4) {
        putchar('Y');
    } else {
        putchar('N');
    }

    seek_to(&cur_hi, &cur_lo, 0x001D, 0xFFF8);
    skip(&cur_hi, &cur_lo, 16);
    if (cur_lo == 8) {
        putchar('Y');
    } else {
        putchar('N');
    }
    if (cur_hi == 0x001E) {
        putchar('Y');
    } else {
        putchar('N');
    }

    putchar('\n');
    return 0;
}
