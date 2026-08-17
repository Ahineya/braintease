// TITLEPIC is stored column-major at x * 200 + y.
// int is 16-bit signed, so x * 200 overflows for x >= 164 (32800).

void putchar(int c);

int main() {
    unsigned short x;
    unsigned short y;
    unsigned short addr;

    x = 0;
    y = 0;
    addr = x * 200 + y;
    if (addr == 0) {
        putchar('Y');
    } else {
        putchar('N');
    }

    x = 100;
    y = 50;
    addr = x * 200 + y;
    if (addr == 20050) {
        putchar('Y');
    } else {
        putchar('N');
    }

    // 164 * 200 = 32800 = 0x8020, just past signed 16-bit max
    x = 164;
    y = 0;
    addr = x * 200 + y;
    if (addr == 0x8020) {
        putchar('Y');
    } else {
        putchar('N');
    }

    // last TITLEPIC pixel: 319 * 200 + 199 = 63999 = 0xF9FF
    x = 319;
    y = 199;
    addr = x * 200 + y;
    if (addr == 0xF9FF) {
        putchar('Y');
    } else {
        putchar('N');
    }

    putchar('\n');
    return 0;
}
