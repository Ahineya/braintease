void putchar(int c);

struct Inner {
    int a;
    int b;
};

struct Outer {
    struct Inner x;
    struct Inner y;
};

int main() {
    struct Outer p;
    p.x.a = 5;
    p.x.b = 0;
    p.y.a = 100;
    p.y.b = 0;
    if (p.x.a == 5 && p.y.a == 100) {
        putchar('Y');
    } else {
        putchar('N');
    }
    putchar('\n');
    return 0;
}
