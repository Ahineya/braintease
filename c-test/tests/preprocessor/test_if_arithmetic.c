void putchar(int c);

#define VERSION 2

#if 1+1 == 2
int ok_sum() { return 1; }
#else
int ok_sum() { return 0; }
#endif

#if VERSION == 1
int ver() { return 0; }
#elif VERSION == 2
int ver() { return 1; }
#else
int ver() { return 0; }
#endif

#if 1 && 0
int both() { return 1; }
#else
int both() { return 0; }
#endif

int main() {
    if (ok_sum()) putchar('Y'); else putchar('N');
    if (ver()) putchar('Y'); else putchar('N');
    if (!both()) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
