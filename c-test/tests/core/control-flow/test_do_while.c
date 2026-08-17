void putchar(int c);

int main() {
    int i;
    int n;

    i = 0;
    n = 0;
    do {
        i = i + 1;
        if (i == 2) continue;
        n = n + 1;
    } while (i < 4);
    if (i == 4) putchar('Y'); else putchar('N');
    if (n == 3) putchar('Y'); else putchar('N');

    i = 0;
    do {
        i = i + 1;
        if (i == 2) break;
    } while (1);
    if (i == 2) putchar('Y'); else putchar('N');

    i = 0;
    do {
        i = 1;
    } while (0);
    if (i == 1) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
