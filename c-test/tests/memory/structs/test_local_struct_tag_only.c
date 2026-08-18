// Block-scope tag definition with no object — c-testsuite 00052.
void putchar(int c);

int main() {
    struct T { int x; };
    {
        struct T s;
        s.x = 0;
        if (s.x == 0) putchar('Y'); else putchar('N');
    }
    putchar('\n');
    return 0;
}
