void putchar(int c);

int main() {
    unsigned long long n = 4294967296ULL;
    int k;
    k = 0;
    while (n > 0) {
        k = k + 1;
        n = n / 10ULL;
        if (k > 20) {
            putchar('F');
            putchar('\n');
            return 0;
        }
    }
    if (k == 10) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
