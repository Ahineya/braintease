void putchar(int c);

int main() {
    int x;
    int y;

    x = 0;
    y = (x = 1, x + 2);
    if (y == 3) putchar('Y'); else putchar('N');
    if (x == 1) putchar('Y'); else putchar('N');

    x = 0;
    y = 0;
    x = (y = 5, 9);
    if (x == 9) putchar('Y'); else putchar('N');
    if (y == 5) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
