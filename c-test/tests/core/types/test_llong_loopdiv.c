void putchar(int c);

int count_digits(unsigned long long n, unsigned long long b) {
    char buf[32];
    int i;

    i = 0;
    if (n == 0) {
        return 1;
    }
    while (n > 0) {
        buf[i] = (char)(n % b);
        i = i + 1;
        n = n / b;
        if (i > 30) {
            return -1;
        }
    }
    return i;
}

int main() {
    int k;
    k = count_digits(4294967296ULL, 10ULL);
    if (k == 10) putchar('Y'); else putchar('N');
    k = count_digits(20ULL, 10ULL);
    if (k == 2) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
