void putchar(int c);

int main() {
    int a;

    a = 15;
    a &= 7;
    if (a == 7) putchar('Y'); else putchar('N');

    a |= 8;
    if (a == 15) putchar('Y'); else putchar('N');

    a ^= 1;
    if (a == 14) putchar('Y'); else putchar('N');

    a <<= 1;
    if (a == 28) putchar('Y'); else putchar('N');

    a >>= 2;
    if (a == 7) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
