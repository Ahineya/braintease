// Forward struct tag, then complete definition — c-testsuite 00044.
// Inner-scope `struct T` must not clobber the outer tag.
void putchar(int c);

struct T;

struct T {
    int x;
};

int main() {
    struct T v;
    { struct T { int z; }; }
    v.x = 2;
    if (v.x == 2) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
