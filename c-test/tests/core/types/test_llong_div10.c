void putchar(int c);

int main() {
    long long a = 20;
    if (a / 10 == 2) putchar('Y'); else putchar('N');
    if (a % 10 == 0) putchar('Y'); else putchar('N');

    unsigned long long u = 20ULL;
    if (u / 10ULL == 2ULL) putchar('Y'); else putchar('N');
    if (u % 10ULL == 0) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
